/* TEST RIG ONLY (not for submission): drives the ring (src/ring.c) through the real harness.
 * Build: copy this file over lb/main.c in a SCRATCH copy of the project, then make; tests/run.sh build.
 * - hash():            the team's hash() from lb/main.c, unchanged (parse, service lookup, CRC32C)
 * - init/on_event/compute_replica: Atieh's code from lb/main.c, unchanged
 * - a stand-in flow table (linear probing, 16-byte entries) and IP-in-IP encapsulation, so the harness's
 *   correctness checks (broken / misplaced / copy failures) exercise the ring for real.
 * - option drain_aware=1: a non-SYN table miss is looked up on a second ring that still holds draining replicas.
 * - option syn_rule=0: disables "a SYN hitting an entry whose replica is not active is re-selected".
 * - prints RINGCAP lines at the end: whether the ring arrays were ever reallocated (on_event allocations).
 * The stand-in flow table's timeouts are rough (web 15 s idle): the web broken flows it shows come from that.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen.h"
#include "parser.h"
#include "ring.h"
#include "crc32c.h"

typedef struct { uint32_t sip; uint16_t sport; uint8_t svc; uint8_t state; uint32_t rep_idx; uint32_t last_ms; } fent_t;
enum { F_EMPTY = 0, F_HALF, F_EST, F_CLOSING };

struct lb {
    gen_config_t cfg;
    ring_t *rings;
    ring_t *rings_drain;   /* drain_aware: same events, but DRAIN does not remove points */
    int drain_aware;
    fent_t *ft; uint32_t ft_mask;
    uint32_t to_half_ms, to_closing_ms, to_est_ms[16];
};
struct pkt_state {
    flow_key_t key;
    uint32_t hash;
    uint32_t replica;
    uint32_t service_id;
    uint8_t tcp_flags;
    uint8_t valid;
};

static lb_t *init(gen_t *g, const gen_config_t *cfg)
{
    lb_t *lb = gen_alloc(g, sizeof(lb_t), 0, 0);
    if (!lb) return NULL;
    memset(lb, 0, sizeof *lb);
    lb->cfg = *cfg;
    const char *pv = gen_param(g, "ring_v");
    unsigned long v = pv ? strtoul(pv, NULL, 10) : 100;
    if (v == 0 || v > 100000) return NULL;
    lb->rings = gen_alloc(g, (cfg->n_services ? cfg->n_services : 1) * sizeof *lb->rings, 0, 0);
    if (!lb->rings) return NULL;
    for (uint32_t k = 0; k < cfg->n_services; k++)
        if (!ring_init(&lb->rings[k], g, &cfg->services[k], (uint32_t)v, cfg->seed)) return NULL;

    const char *da = gen_param(g, "drain_aware");
    lb->drain_aware = da && atoi(da);
    if (lb->drain_aware) {
        lb->rings_drain = gen_alloc(g, (cfg->n_services ? cfg->n_services : 1) * sizeof *lb->rings, 0, 0);
        if (!lb->rings_drain) return NULL;
        for (uint32_t k = 0; k < cfg->n_services; k++) {
            /* initial draining replicas count as active in this ring */
            service_t s2 = cfg->services[k];
            replica_t *tmp = gen_alloc(g, s2.n_replicas * sizeof *tmp + 1, 0, 0);
            for (uint32_t i = 0; i < s2.n_replicas; i++) {
                tmp[i] = s2.replicas[i];
                if (tmp[i].state == REPLICA_STATE_DRAINING) tmp[i].state = REPLICA_STATE_ACTIVE;
            }
            s2.replicas = tmp;
            if (!ring_init(&lb->rings_drain[k], g, &s2, (uint32_t)v, cfg->seed)) return NULL;
            gen_mfree(g, tmp);
        }
    }
    const char *fs = gen_param(g, "ft_log2");
    int lg = fs ? atoi(fs) : 20;
    lb->ft = gen_alloc(g, ((size_t)1 << lg) * sizeof(fent_t), 64, 1);
    if (!lb->ft) { fprintf(stderr, "ft alloc failed\n"); return NULL; }
    memset(lb->ft, 0, ((size_t)1 << lg) * sizeof(fent_t));
    lb->ft_mask = (1u << lg) - 1;
    lb->to_half_ms = 10000; lb->to_closing_ms = 3000;
    for (uint32_t k = 0; k < 16; k++) lb->to_est_ms[k] = 200000;
    for (uint32_t k = 0; k < cfg->n_services && k < 16; k++)
        if (cfg->services[k].vip == 0xC6336414u) lb->to_est_ms[k] = 60000;      /* video */
        else if (cfg->services[k].vip == 0xC633641Eu) lb->to_est_ms[k] = 15000; /* web */
    return lb;
}

static void on_event(lb_t *lb, const gen_event_t *ev)
{
    if (ev->service >= lb->cfg.n_services)
        return;
    if (!ring_event(&lb->rings[ev->service], (int)ev->type, ev->ipv4))
        fprintf(stderr, "ring: gen_alloc failed on a replica event\n");
    if (lb->drain_aware && ev->type != REPLICA_DRAIN)
        ring_event(&lb->rings_drain[ev->service], (int)ev->type, ev->ipv4);
}

/* the team's hash(), unchanged */
static void hash(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask)
{
    for (uint8_t i = 0; i < n; i++) {
        if (!(mask & (1ull << i)))
            continue;
        memset(&s[i], 0, sizeof(s[i]));
        const uint8_t *f = p[i]->buf + p[i]->off;
        uint16_t len = p[i]->len;
        if (!parse_flow(f, len, &s[i].key, &s[i].tcp_flags))
            continue;
        flow_key_t *key = &s[i].key;
        for (uint32_t y = 0; y < lb->cfg.n_services; y++) {
            service_t service = lb->cfg.services[y];
            if (service.proto == key->protocol && service.vip == key->dst_ip && service.port == key->dst_port) {
                s[i].service_id = y;
                s[i].valid = 1;
                break;
            }
        }
        if (!s[i].valid)
            continue;
        uint64_t ips = ((uint64_t)key->src_ip << 32) | key->dst_ip;
        uint32_t ports = ((uint32_t)key->src_port << 16) | key->dst_port;
        uint32_t h = crc32c_u64(~0u, lb->cfg.seed);
        h = crc32c_u64(h, ips);
        h = crc32c_u32(h, ports);
        h = crc32c_u8(h, key->protocol);
        s[i].hash = h;
    }
}

static int syn_rule = 1;   /* syn_rule=0 to see the misplaced flows it avoids */
static int is_syn(const pkt_state_t *s) { return (s->tcp_flags & 0x12) == 0x02; }

static uint32_t timeout_of(const lb_t *lb, const fent_t *e)
{
    return e->state == F_HALF ? lb->to_half_ms : e->state == F_CLOSING ? lb->to_closing_ms : lb->to_est_ms[e->svc & 15];
}

static uint64_t lookup(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask)
{
    uint64_t hit = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (!(mask & (1ull << i))) continue;
        uint32_t now = (uint32_t)(p[i]->ts_ns / 1000000u);
        uint32_t j = s[i].hash & lb->ft_mask;
        for (int probe = 0; probe < 256; probe++, j = (j + 1) & lb->ft_mask) {
            fent_t *e = &lb->ft[j];
            if (e->state == F_EMPTY) break;
            if (e->sip != s[i].key.src_ip || e->sport != s[i].key.src_port || e->svc != s[i].service_id) continue;
            if (now - e->last_ms > timeout_of(lb, e)) break;               /* expired: a miss */
            ring_t *r = &lb->rings[e->svc];
            if (r->reps[e->rep_idx].state == REPLICA_STATE_DOWN) break;     /* lost flow: recompute */
            if (syn_rule && is_syn(&s[i]) && ring_state(r, e->rep_idx) != REPLICA_STATE_ACTIVE) break; /* retransmitted SYN: a new flow decision */
            s[i].replica = ring_ip(r, e->rep_idx);
            e->last_ms = now;
            if (s[i].tcp_flags & 0x05) e->state = F_CLOSING;                /* FIN or RST */
            else if (e->state == F_HALF && (s[i].tcp_flags & 0x10)) e->state = F_EST;
            hit |= 1ull << i;
            break;
        }
    }
    return hit;
}

/* Atieh's compute_replica, unchanged when drain_aware=0 */
static uint64_t compute_replica(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask)
{
    (void)p;
    uint64_t ok = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (!(mask & (1ull << i)) || !s[i].valid)
            continue;
        const ring_t *r = (lb->drain_aware && !is_syn(&s[i])) ? &lb->rings_drain[s[i].service_id]
                                                              : &lb->rings[s[i].service_id];
        uint32_t ip = ring_lookup(r, s[i].hash);
        if (!ip)
            continue;
        s[i].replica = ip;
        ok |= 1ull << i;
    }
    return ok;
}

static uint64_t insert(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask)
{
    uint64_t ok = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (!(mask & (1ull << i))) continue;
        uint32_t now = (uint32_t)(p[i]->ts_ns / 1000000u);
        ring_t *r = &lb->rings[s[i].service_id];
        uint32_t idx = ring_replica_id(r, s[i].replica);
        if (idx == RING_NO_ID) continue;    /* drain_aware ring may know a replica the main ring has no slot for: impossible here */
        uint32_t j = s[i].hash & lb->ft_mask, target = UINT32_MAX;
        for (int probe = 0; probe < 256; probe++, j = (j + 1) & lb->ft_mask) {
            fent_t *e = &lb->ft[j];
            if (e->state == F_EMPTY) { if (target == UINT32_MAX) target = j; break; }
            if (e->sip == s[i].key.src_ip && e->sport == s[i].key.src_port && e->svc == s[i].service_id) { target = j; break; }
            if (target == UINT32_MAX && now - e->last_ms > timeout_of(lb, e)) target = j;
        }
        if (target == UINT32_MAX) continue;
        fent_t *e = &lb->ft[target];
        e->sip = s[i].key.src_ip; e->sport = s[i].key.src_port; e->svc = (uint8_t)s[i].service_id;
        e->rep_idx = idx; e->last_ms = now;
        e->state = (s[i].tcp_flags & 0x05) ? F_CLOSING : is_syn(&s[i]) ? F_HALF : F_EST;
        ok |= 1ull << i;
    }
    return ok;
}

static uint32_t replica_of(const pkt_state_t *s) { return s->replica; }

static const lb_ops_t ops = {
    .state_size = sizeof(pkt_state_t), .init = init, .on_event = on_event, .hash = hash,
    .lookup = lookup, .compute_replica = compute_replica, .insert = insert, .replica_of = replica_of,
};

static uint16_t csum(const uint8_t *h)
{
    uint32_t sum = 0;
    for (int i = 0; i < 20; i += 2) sum += ((uint32_t)h[i] << 8) | h[i + 1];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static void encap(const lb_t *lb, pkt_t *pk, uint32_t dst)
{
    uint8_t *f = pk->buf + pk->off - 20;
    uint16_t inner_len = (uint16_t)((f[20 + 14 + 2] << 8) | f[20 + 14 + 3]);
    memcpy(f, lb->cfg.zone_mac, 6); memcpy(f + 6, lb->cfg.int_mac, 6); f[12] = 0x08; f[13] = 0x00;
    uint8_t *ip = f + 14;
    uint16_t tot = (uint16_t)(inner_len + 20);
    ip[0] = 0x45; ip[1] = 0; ip[2] = (uint8_t)(tot >> 8); ip[3] = (uint8_t)tot; ip[4] = ip[5] = ip[6] = ip[7] = 0;
    ip[8] = 64; ip[9] = 4; ip[10] = ip[11] = 0;
    uint32_t src = lb->cfg.int_ipv4;
    ip[12] = (uint8_t)(src >> 24); ip[13] = (uint8_t)(src >> 16); ip[14] = (uint8_t)(src >> 8); ip[15] = (uint8_t)src;
    ip[16] = (uint8_t)(dst >> 24); ip[17] = (uint8_t)(dst >> 16); ip[18] = (uint8_t)(dst >> 8); ip[19] = (uint8_t)dst;
    uint16_t c = csum(ip); ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)c;
    pk->off = (uint16_t)(pk->off - 20); pk->len = (uint16_t)(pk->len + 20);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    gen_t *g = gen_open(argv[1], argc > 2 ? argv[2] : "", &ops);
    if (!g) return 1;
    lb_t *lb = gen_lb(g);
    void *init_reps[16], *init_pts[16];
    for (uint32_t k = 0; k < lb->cfg.n_services && k < 16; k++) { init_reps[k] = lb->rings[k].reps; init_pts[k] = lb->rings[k].pts; }
    { const char *sr = gen_param(g, "syn_rule"); if (sr) syn_rule = atoi(sr); }
    pkt_t *p[64];
    pkt_state_t s[64];
    while (gen_running(g)) {
        uint8_t n = gen_rx_batch(g, GEN_NIC_EXT, p, 64);
        if (n == 0) continue;
        uint64_t all = n == 64 ? ~0ull : (1ull << n) - 1;
        gen_hash(g, lb, p, n, s, all);
        uint64_t ok = 0;
        for (uint8_t i = 0; i < n; i++) if (s[i].valid) ok |= 1ull << i;
        uint64_t hit = gen_lookup(g, lb, p, n, s, ok);
        uint64_t got = gen_replica(g, lb, p, n, s, ok & ~hit);
        gen_insert(g, lb, p, n, s, got);
        uint64_t send = hit | got;
        for (uint8_t i = 0; i < n; i++) if (send & (1ull << i)) encap(lb, p[i], s[i].replica);
        gen_tx_batch(g, GEN_NIC_INT, p, n, send);
        gen_drop(g, p, n, all & ~send);
    }
    for (uint32_t k = 0; k < lb->cfg.n_services; k++) {
        ring_t *r = &lb->rings[k];
        fprintf(stderr, "RINGCAP svc %u: reps %u/%u pts %u/%u reps_ptr_same=%d pts_ptr_same=%d\n", k, r->n_reps, r->cap_reps,
                r->n_pts, r->cap_pts, (void *)r->reps == init_reps[k], (void *)r->pts == init_pts[k]);
    }
    gen_close(g);
    return 0;
}

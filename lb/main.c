/* lb/main.c: the LB's main program: its stages (lb_ops_t) and its loop.
 *
 *   build/lb SCENARIO "PARAMS"
 *
 * runs your LB on SCENARIO with the parameters PARAMS (key=value,...), then
 * exits; the harness writes the results file (results=PATH, default
 * results.csv).  The contest runs
 *
 *   build/lb mixed_flood.scn "$(build/autotune),$(cat config.params),KEYS"
 *
 * KEYS being the harness keys of the run (mode, results, ...).  A key given
 * twice makes gen_open fail: exit with an error then, as below.
 *
 * Put your components (the stages, your tables) in src/: build/autotune links
 * them too.  README.md documents the harness.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen.h"
#include "parser.h"
#include "ring.h"
#include "flow_table.h"
#include "tabhash.h"

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_ACK 0x10


/* A connection's first packet: SYN set, ACK not set (a SYN-ACK never reaches the LB: replies bypass it). */
static inline int is_syn(uint8_t f) { return (f & (TCP_SYN | TCP_ACK)) == TCP_SYN; }



struct lb {
    gen_config_t cfg; // copy of gen_config_t
    ring_t *rings;    /* one consistent-hash ring per service, cfg.services[k] -> rings[k] (Atieh) */
    flow_table_t *ft; /* cuckoo flow table (Samuel) */
    tabhash_t *th;    /* simple tabulation hashing tables, filled from cfg.seed */
    uint8_t hashfn;   /* HASH_TAB or HASH_CRC */
    svc_timeouts_t *to; /* timeouts per service, indexed by service_id */
    /* Prefetching (E1, Atieh): where the stages ask the CPU to load cache lines ahead, each 0 (off) or on:
       pf_pkt=1 the packet headers hash parses; pf_bucket=1 the first bucket of the index lookup reads, =2 both
       buckets; pf_entry=1 the entries the first bucket points to with the key's signature, =2 those of both
       buckets. And how far ahead, pf_dist: 0 = a stage first asks for its whole batch, then works on it; D > 0 =
       while it works on packet i it asks for packet i + D (buckets for i + 2D when pf_entry needs them first), so
       that at most a few packets' lines are on their way at once. */
    uint8_t pf_pkt, pf_bucket, pf_entry;
    uint32_t pf_dist;
};

/* pf_pkt (Atieh): the 54 bytes of Ethernet + IPv4 + TCP headers from off, which may span two cache lines */
static inline void prefetch_headers(const pkt_t *pk)
{
    const uint8_t *f = pk->buf + pk->off;
    __builtin_prefetch(f);
    __builtin_prefetch(f + 53);
}
struct pkt_state {
    flow_key_t key;
    uint64_t hash;
    ft_loc_t loc;   /* the key's buckets and signature, from the hash, computed once in hash() (Atieh) */
    uint32_t replica;
    uint32_t service_id;
    uint8_t tcp_flags;
    uint8_t valid;
};


/* State of a flow inserted on this packet: a SYN opens it, a FIN or RST closes it, anything else is a flow
   already in progress (from another LB, or evicted earlier). UDP has no handshake: established. */
static uint8_t first_state(const pkt_state_t *s)
{
    if (s->key.protocol != 6)
        return FS_ESTABLISHED;
    if (s->tcp_flags & (TCP_FIN | TCP_RST))
        return FS_CLOSING;
    if (is_syn(s->tcp_flags))
        return FS_HALF_OPEN;
    return FS_ESTABLISHED;
}


/* The harness does not check init's result: it goes on to call the copy's hash with a NULL lb and crashes
   (segfault in hash). So a setup error must end the run here, with a message, rather than return NULL. */
static void init_fail(const char *msg)
{
    fprintf(stderr, "init: %s\n", msg);
    exit(1);
}

/* A whole-number parameter in [min, max], def if absent; a bad value ends the run (rejects "", "abc", "100x"). */
static unsigned long param_ul(gen_t *g, const char *key, unsigned long def, unsigned long min, unsigned long max)
{
    const char *v = gen_param(g, key);
    if (!v)
        return def;
    char *end = NULL;
    unsigned long x = strtoul(v, &end, 10);
    if (end == v || *end != '\0' || x < min || x > max) {
        fprintf(stderr, "init: %s=%s: must be a whole number between %lu and %lu\n", key, v, min, max);
        exit(1);
    }
    return x;
}

#define MS 1000000ull           /* ns per ms */
#define TO_MAX_MS 86400000ul    /* timeouts: at most a day */

/* Timeouts, in ms: to_half_ms, to_est_ms, to_fin_ms for every service, then optional overrides per service,
   keyed by its VIP */
static void init_timeouts(gen_t *g, lb_t *lb)
{
    static const char *const names[3] = { "to_half_ms", "to_est_ms", "to_fin_ms" };
    static const unsigned long defaults[3] = { 1000, 130000, 2000 };
    unsigned long base[3];
    for (int k = 0; k < 3; k++)
        base[k] = param_ul(g, names[k], defaults[k], 1, TO_MAX_MS);

    uint32_t n = lb->cfg.n_services;
    lb->to = gen_alloc(g, (n ? n : 1) * sizeof *lb->to, 0, 0);
    if (!lb->to)
        init_fail("gen_alloc for the timeouts failed");
    for (uint32_t i = 0; i < n; i++) {
        uint32_t vip = lb->cfg.services[i].vip;
        unsigned long ms[3];
        for (int k = 0; k < 3; k++) {
            char key[64];
            snprintf(key, sizeof key, "%s_%u.%u.%u.%u", names[k],
                     vip >> 24, (vip >> 16) & 0xff, (vip >> 8) & 0xff, vip & 0xff);
            ms[k] = param_ul(g, key, base[k], 1, TO_MAX_MS);
        }
        lb->to[i].half = ms[0] * MS;
        lb->to[i].est = ms[1] * MS;
        lb->to[i].fin = ms[2] * MS;
    }
}

//modify by Sam
static lb_t *init(gen_t *g, const gen_config_t *cfg)
{
    lb_t *lb = gen_alloc(g, sizeof(lb_t), 0, 0);
    if (!lb)
        init_fail("gen_alloc for LB failed");
    memset(lb, 0, sizeof *lb);
    lb->cfg = *cfg;

    /* Replica selection (Atieh): one ring per service, ring_v points per replica (default 100). */
    uint32_t v = (uint32_t)param_ul(g, "ring_v", 100, 1, 100000);
    lb->rings = gen_alloc(g, (cfg->n_services ? cfg->n_services : 1) * sizeof *lb->rings, 0, 0);
    if (!lb->rings)
        init_fail("gen_alloc for rings failed");
    for (uint32_t k = 0; k < cfg->n_services; k++)
        if (!ring_init(&lb->rings[k], g, &cfg->services[k], v, cfg->seed))
            init_fail("gen_alloc for a service's ring failed (ring_v too large for the memory budget?)");

    const char *hf = gen_param(g, "hashfn");
    if (!hf || strcmp(hf, "tab") == 0) {
        lb->hashfn = HASH_TAB;
    } else if (strcmp(hf, "crc") == 0) {
        lb->hashfn = HASH_CRC;
    } else {
        init_fail("bad hashfn value: tab or crc");
    }

    lb->th = gen_alloc(g, sizeof(tabhash_t), 64, 0);
    if (!lb->th) {
        init_fail("gen_alloc for the tabulation tables failed");
    }
    tabhash_init(lb->th, cfg->seed);

    uint32_t nb = (uint32_t)param_ul(g, "ft_buckets", 65536, 2, UINT32_MAX);
    uint32_t bs = (uint32_t)param_ul(g, "bucket_size", 10, 2, 64);
    uint32_t md = (uint32_t)param_ul(g, "max_depth", 5, 2, 16);
    uint32_t dfs = (uint32_t)param_ul(g, "dfs_budget", 64, 2, 100000);
    // signature width: 0 = every bit the entry index leaves free
    uint8_t sb = (uint8_t)param_ul(g, "sig_bits", 0, 0, FT_MAX_SIG_BITS);

    init_timeouts(g, lb);

    lb->ft = ft_init(g, nb, bs, md, dfs, sb,
                    lb->hashfn, lb->th, cfg->seed, lb->to);

    if (!lb->ft) {
        init_fail("flow table initialization failed");
    }

    /* Prefetching (E1, Atieh): off unless the params turn it on, so that it can be measured on and off */
    lb->pf_pkt = (uint8_t)param_ul(g, "pf_pkt", 0, 0, 1);
    lb->pf_bucket = (uint8_t)param_ul(g, "pf_bucket", 0, 0, 2);
    lb->pf_entry = (uint8_t)param_ul(g, "pf_entry", 0, 0, 2);
    lb->pf_dist = (uint32_t)param_ul(g, "pf_dist", 0, 0, 64);
    return lb;
}

/* Made by Atieh: replica events update the service's ring (incrementally, see src/ring.c). */
static void on_event(lb_t *lb, const gen_event_t *ev)
{
    if (ev->service >= lb->cfg.n_services)
        return;
    /* ring_init reserved room for the published scenarios' churn, so this does not allocate there. If it
       has to and gen_alloc fails, this instance's ring no longer matches the copy's: say so loudly. */
    if (!ring_event(&lb->rings[ev->service], (int)ev->type, ev->ipv4))
        fprintf(stderr, "ring: gen_alloc failed on a replica event (service %u, type %d): ring not updated\n",
                ev->service, (int)ev->type);
}

/*Made By ludovic and next modify by samuel*/

static void hash(lb_t *lb, pkt_t **p, uint8_t n,
                 pkt_state_t *s, uint64_t mask)
{
    /* pf_pkt (Atieh): the packet buffers live in a pool far larger than the cache, so reading each packet's headers
       is a cache miss. Ask for them ahead (all of them first with pf_dist=0, else pf_dist packets ahead); the loop
       then parses packets whose headers are already on their way. */
    uint32_t d = lb->pf_dist;
    if (lb->pf_pkt) {
        uint32_t first = d == 0 || d > n ? n : d;
        for (uint32_t i = 0; i < first; i++)
            if (mask & (1ull << i))
                prefetch_headers(p[i]);
    }

    for (uint8_t i = 0; i < n; i++) {
        if (lb->pf_pkt && d && i + d < n && (mask & (1ull << (i + d))))
            prefetch_headers(p[i + d]);
        if (!(mask & (1ull << i)))
            continue;

        // Initialize packet state
        memset(&s[i], 0, sizeof(s[i]));
        const uint8_t *f = p[i]->buf + p[i]->off;
        uint16_t len = p[i]->len;

        // Parse Ethernet, IPv4 and TCP/UDP
        if (!parse_flow(f, len, &s[i].key, &s[i].tcp_flags))
            continue;
        flow_key_t *key = &s[i].key;

        // Find the corresponding service
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
        // Hash the 5-tuple: simple tabulation (default) or CRC32C (baseline)
        s[i].hash = lb->hashfn == HASH_TAB ? tabhash_flow(lb->th, key) : crc_flow(lb->cfg.seed, key);

        /* Where the key can be in the flow table, once for lookup, insert and the prefetches (Atieh) */
        ft_locate(lb->ft, s[i].hash, &s[i].loc);

        /* pf_bucket with pf_dist=0 (Atieh): the two buckets lookup will read for this packet, asked for now so that
           they arrive while hash works on the rest of the batch (with pf_dist > 0, lookup asks for them) */
        if (lb->pf_bucket && d == 0)
            ft_prefetch_buckets(lb->ft, &s[i].loc, lb->pf_bucket == 2);
    }
}


/* (Atieh) What the flow table needs from replica selection -- measured with a stand-in table:
 *   - store the replica as its id, ring_replica_id(&lb->rings[svc], s[i].replica) at insert (16 bits are
 *     enough), and read it back with ring_ip(); ids never change during a run;
 *   - on a hit, if ring_state(r, id) == REPLICA_STATE_DOWN: treat it as a miss (the flow is lost; its
 *     packets must go elsewhere, and its client's retransmitted SYN must not go back to the dead replica);
 *   - on a hit by a SYN (SYN set, ACK not set), if ring_state(r, id) != REPLICA_STATE_ACTIVE: treat it as a
 *     miss too. A retransmitted SYN is a new-flow decision: without this, a SYN whose replica failed
 *     silently and was then drained is sent to a draining replica (44 misplaced flows on events.scn).
 *   With neither check, tests/failover.scn and tests/churn.scn fail (52 and 83 misplaced flows). */
 //modify by sam 
static uint64_t lookup(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask)
{
    /* Prefetching (Atieh). pf_dist=0: pf_entry reads every packet's buckets first (in cache already with pf_bucket,
       which hash asked for) and asks for the entries they point to with its signature; the loop then finds them
       arriving or arrived. pf_dist=D: while it looks packet i up, it asks for the buckets of packet i + D (of
       i + 2D with pf_entry, so that they have arrived when the entries of that packet are asked for, D packets
       later) and for the entries of packet i + D. */
    uint32_t d = lb->pf_dist;
    uint32_t bd = lb->pf_entry ? 2 * d : d;
#define PF_OK(j) ((j) < n && (mask & (1ull << (j))) && s[(j)].valid)
    if (d == 0) {
        if (lb->pf_entry)
            for (uint32_t i = 0; i < n; i++)
                if (PF_OK(i))
                    ft_prefetch_entries(lb->ft, &s[i].loc, lb->pf_entry == 2);
    } else if (lb->pf_bucket) {
        for (uint32_t i = 0; i < bd && i < n; i++)
            if (PF_OK(i))
                ft_prefetch_buckets(lb->ft, &s[i].loc, lb->pf_bucket == 2);
    }

    uint64_t hit = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (d) {
            if (lb->pf_bucket && PF_OK(i + bd))
                ft_prefetch_buckets(lb->ft, &s[i + bd].loc, lb->pf_bucket == 2);
            if (lb->pf_entry && PF_OK(i + d))
                ft_prefetch_entries(lb->ft, &s[i + d].loc, lb->pf_entry == 2);
        }
        if (!(mask & (1ull << i)) || !s[i].valid)
            continue;
        uint64_t now = p[i]->ts_ns;
        flow_entry_t *e = ft_find_at(lb->ft, &s[i].key, &s[i].loc, now);
        if (!e)
            continue;                                       /* miss: not in the table, or expired */

        const ring_t *r = &lb->rings[s[i].service_id];
        uint8_t f = s[i].tcp_flags;
        int syn = s[i].key.protocol == 6 && is_syn(f);
        uint8_t rs = ring_state(r, e->replica);
        /* Atieh's two rules: a down replica, or a SYN (a new-flow decision) to a replica that is not active,
           is a miss. The entry stays: insert overwrites it with compute_replica's choice. */
        if (rs == REPLICA_STATE_DOWN || (syn && rs != REPLICA_STATE_ACTIVE))
            continue;
        s[i].replica = ring_ip(r, e->replica);

        /* Lifecycle (brief, item 5): refresh the last-seen time; FIN or RST starts the closing grace period,
           which later packets do not extend. */
        /* last_seen is refreshed by every packet except those of a closing flow: there it stays the time of the
           FIN or RST, which starts the grace period (is_expired, src/flow_table.c) */
        if (s[i].key.protocol == 6 && (f & (TCP_FIN | TCP_RST))) {
            if (e->state != FS_CLOSING) {
                e->state = FS_CLOSING;
                if (now > e->last_seen)
                    e->last_seen = now;
            }
        } else if (syn) {
            e->state = FS_HALF_OPEN;
            if (now > e->last_seen)
                e->last_seen = now;
        } else if (e->state != FS_CLOSING) {
            if (e->state == FS_HALF_OPEN &&
                (s[i].key.protocol != 6 || (f & TCP_ACK))) {
                e->state = FS_ESTABLISHED;
            }
            if (now > e->last_seen)
                e->last_seen = now;
        }
        hit |= 1ull << i;
    }
#undef PF_OK
    return hit;
}

/* Made by Atieh: consistent hashing over the service's active replicas.
   Works on its own (the harness's copy calls it right after hash, without lookup). */
static uint64_t compute_replica(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask)
{
    (void)p;
    uint64_t ok = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (!(mask & (1ull << i)) || !s[i].valid)
            continue;
        uint32_t ip = ring_lookup(&lb->rings[s[i].service_id], s[i].hash);
        if (!ip)
            continue;   /* no active replica in this service */
        s[i].replica = ip;
        ok |= 1ull << i;
    }
    return ok;
}

/* Stores the flows compute_replica gave a replica: new flows (SYN), flows first seen mid-flow (from another LB, or
   evicted earlier), and flows whose stored replica lookup rejected. ft_insert is an upsert, so two packets of one
   new flow in the same batch (same hash, same ring: same replica) store a single entry. A failed insert only
   clears the packet's bit: the main loop forwards it anyway, by consistent hashing (brief, item 5). */
//modify by sam
static uint64_t insert(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask)
{
    uint64_t stored = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (!(mask & (1ull << i)) || !s[i].valid)
            continue;
        uint32_t svc = s[i].service_id;
        uint32_t id = ring_replica_id(&lb->rings[svc], s[i].replica);
        if (id == RING_NO_ID)
            continue;                       /* cannot happen: compute_replica returns a replica of the ring */
        uint64_t now = p[i]->ts_ns;
        uint8_t st = first_state(&s[i]);

        if (ft_insert_at(lb->ft, &s[i].key, id, st, &s[i].loc,
                    svc, now))
            stored |= 1ull << i;
    }
    return stored;
}

static uint32_t replica_of(const pkt_state_t *s)
{
    return s->replica;
}

/* IP-in-IP encapsulation, ported from Atieh's test rig (scripts/tests_ring/e2e_main.c):
   the new Ethernet and outer IPv4 headers go over the 20 bytes of headroom and the old Ethernet header; the inner
   packet does not move. */
static uint16_t ip_checksum(const uint8_t *h)
{
    uint32_t sum = 0;
    for (int i = 0; i < 20; i += 2)
        sum += ((uint32_t)h[i] << 8) | h[i + 1];
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static void put32(uint8_t *b, uint32_t x)
{
    b[0] = (uint8_t)(x >> 24); b[1] = (uint8_t)(x >> 16); b[2] = (uint8_t)(x >> 8); b[3] = (uint8_t)x;
}

static void encap(const lb_t *lb, pkt_t *pk, uint32_t dst)
{
    uint8_t *f = pk->buf + pk->off - 20;                        /* the new frame starts in the headroom */
    uint16_t inner_len = rd16(f + 20 + 14 + 2);                 /* the inner IPv4 total length */
    memcpy(f, lb->cfg.zone_mac, 6);
    memcpy(f + 6, lb->cfg.int_mac, 6);
    f[12] = 0x08; f[13] = 0x00;
    uint8_t *ip = f + 14;
    uint16_t tot = (uint16_t)(inner_len + 20);
    ip[0] = 0x45; ip[1] = 0;
    ip[2] = (uint8_t)(tot >> 8); ip[3] = (uint8_t)tot;
    ip[4] = ip[5] = ip[6] = ip[7] = 0;                          /* ID, flags, fragment offset */
    ip[8] = 64; ip[9] = 4;                                      /* TTL, protocol IP-in-IP */
    ip[10] = ip[11] = 0;
    put32(ip + 12, lb->cfg.int_ipv4);
    put32(ip + 16, dst);
    uint16_t c = ip_checksum(ip);
    ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)c;
    pk->off = (uint16_t)(pk->off - 20);
    pk->len = (uint16_t)(pk->len + 20);
}

static const lb_ops_t ops = {
    .state_size = sizeof(pkt_state_t),
    .init = init,
    .on_event = on_event,
    .hash = hash,
    .lookup = lookup,
    .compute_replica = compute_replica,
    .insert = insert,
    .replica_of = replica_of,
};

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s SCENARIO [PARAMS]\n", argv[0]);
        return 2;
    }
    gen_t *g = gen_open(argv[1], argc > 2 ? argv[2] : "", &ops);
    if (!g) return 1;
    lb_t *lb =gen_lb(g);
    pkt_t *p[64];
    pkt_state_t s[64];
    while (gen_running(g)) {
        /* Tunnelled replies (bonus, web_tunnel=1) are not handled: drop whatever reaches the internal NIC. */
        uint8_t m = gen_rx_batch(g, GEN_NIC_INT, p, 64);
        if (m)
            gen_drop(g, p, m, ~0ull);

        uint8_t n = gen_rx_batch(g, GEN_NIC_EXT, p, 64);
        if (n == 0)
            continue;
        uint64_t all = n == 64 ? ~0ull : (1ull << n) - 1;
        gen_hash(g, lb, p, n, s, all);
        uint64_t ok = 0;                                        /* the packets of a service */
        for (uint8_t i = 0; i < n; i++) {
            if (s[i].valid) {
                ok |= (1ull << i);
            }
        }
        uint64_t hit = gen_lookup(g, lb, p, n, s, ok);
        uint64_t got = gen_replica(g, lb, p, n, s, ok & ~hit);
        gen_insert(g, lb, p, n, s, got);                        /* sent even if the insert fails */
        uint64_t send = hit | got;
        for (uint8_t i = 0; i < n; i++)
            if (send & (1ull << i))
                encap(lb, p[i], s[i].replica);
        gen_tx_batch(g, GEN_NIC_INT, p, n, send);
        gen_drop(g, p, n, all & ~send);
    }
#if FT_STATS
    /* E1: the main LB's flow-table counters, raw, on one line of stderr (key=value), with the table's layout and
       its occupancy at the end. scripts/tests_ft/false_matches.sh parses it and compares with the theory. */
    const flow_table_t *ft = lb->ft;
    const ft_stats_t *st = &ft->stats;
    uint64_t used = 0;
    for (uint32_t i = 0; i < ft->capacity; i++)
        used += ft->slots[i] != 0;
    fprintf(stderr, "ft_stats: sig_bits=%u num_buckets=%u bucket_size=%u capacity=%u slots_used=%llu lookups=%llu "
            "hits=%llu slots_checked=%llu sig_matches=%llu false_matches=%llu\n",
            ft->sig_bits, ft->num_buckets, ft->bucket_size, ft->capacity, (unsigned long long)used,
            (unsigned long long)st->lookups, (unsigned long long)st->hits, (unsigned long long)st->slots_checked,
            (unsigned long long)st->sig_matches, (unsigned long long)st->false_matches);

    fprintf(stderr, "\n--- E1 statistics ---\n");

    fprintf(stderr, "Occupied slots: %u / %u\n",
            st->occupied_slots, ft->capacity);

    if (st->first_failure_seen)
    {
        double load_factor =
            100.0 * st->first_failure_occupied / ft->capacity;

        fprintf(stderr, "First failure occupied: %u\n",
                st->first_failure_occupied);

        fprintf(stderr, "First failure load factor: %.2f%%\n",
                load_factor);
    }
    else
    {
        fprintf(stderr, "No insertion failure\n");
    }

    fprintf(stderr, "\nLoad range,Insertions,Displacements,Average\n");

    for (int i = 0; i < 10; i++)
    {
        double average = 0.0;

        if (st->insertions_by_load[i] > 0)
        {
            average = (double)st->displacements_by_load[i]
                    / st->insertions_by_load[i];
        }

        fprintf(stderr, "%d-%d%%,%llu,%llu,%.6f\n",
                i * 10,
                (i + 1) * 10,
                (unsigned long long)st->insertions_by_load[i],
                (unsigned long long)st->displacements_by_load[i],
                average);
    }
#endif
    gen_close(g);
    return 0;
}

/* lb/main.c: your LB's main loop (a stub: it drops every packet).
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
#include "crc32c.h"
#include "tabhash.h"

/* Flow hash function, chosen with the param hashfn=tab (default) or hashfn=crc (the E2 baseline). */
enum { HASH_TAB, HASH_CRC };

struct lb {
    gen_config_t cfg; // copy of gen_config_t
    ring_t *rings;    /* one consistent-hash ring per service, cfg.services[k] -> rings[k] (Atieh) */
    // TODO: define flow table, hash table
    tabhash_t *th;    /* simple tabulation hashing tables, filled from cfg.seed */
    uint8_t hashfn;   /* HASH_TAB or HASH_CRC */
};
struct pkt_state {
    flow_key_t key;
    uint64_t hash;
    uint32_t replica;
    uint32_t service_id;
    uint8_t tcp_flags;
    uint8_t valid;
};

/* The harness does not check init's result: it goes on to call the copy's hash with a NULL lb and crashes
   (segfault in hash). So a setup error must end the run here, with a message, rather than return NULL. */
static void init_fail(const char *msg)
{
    fprintf(stderr, "init: %s\n", msg);
    exit(1);
}

static lb_t *init(gen_t *g, const gen_config_t *cfg)
{
    lb_t *lb = gen_alloc(g, sizeof(lb_t), 0, 0);
    if (!lb)
        init_fail("gen_alloc for LB failed");
    lb->cfg = *cfg;

    /* Replica selection (Atieh): one ring per service, ring_v points per replica (default 100). */
    const char *pv = gen_param(g, "ring_v");
    char *end = NULL;
    unsigned long v = pv ? strtoul(pv, &end, 10) : 100;
    if (v == 0 || v > 100000 || (pv && (end == pv || *end != '\0')))   /* rejects "", "abc", "100x" */
        init_fail("ring_v must be a whole number between 1 and 100000");
    lb->rings = gen_alloc(g, (cfg->n_services ? cfg->n_services : 1) * sizeof *lb->rings, 0, 0);
    if (!lb->rings)
        init_fail("gen_alloc for rings failed");
    for (uint32_t k = 0; k < cfg->n_services; k++)
        if (!ring_init(&lb->rings[k], g, &cfg->services[k], (uint32_t)v, cfg->seed))
            init_fail("gen_alloc for a service's ring failed (ring_v too large for the memory budget?)");

    /* The harness does not check init's result, so a setup error ends the run here, with a message. */
    const char *hf = gen_param(g, "hashfn");
    if (!hf || strcmp(hf, "tab") == 0) {
        lb->hashfn = HASH_TAB;
    } else if (strcmp(hf, "crc") == 0) {
        lb->hashfn = HASH_CRC;
    } else {
        fprintf(stderr, "bad hashfn=%s: tab or crc\n", hf);
        exit(1);
    }

    lb->th = gen_alloc(g, sizeof(tabhash_t), 64, 0);
    if (!lb->th) {
        fprintf(stderr, "gen_alloc for the tabulation tables failed\n");
        exit(1);
    }
    tabhash_init(lb->th, cfg->seed);
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

/* CRC32C of the 5-tuple, seeded: the E2 baseline (32 bits, zero-extended). */
static inline uint64_t crc_flow(uint64_t seed, const flow_key_t *key)
{
    uint64_t ips = ((uint64_t)key->src_ip << 32) | key->dst_ip;
    uint32_t ports = ((uint32_t)key->src_port << 16) | key->dst_port;

    uint32_t h = crc32c_u64(~0u, seed);
    h = crc32c_u64(h, ips);
    h = crc32c_u32(h, ports);
    h = crc32c_u8(h, key->protocol);
    return h;
}

/*Made By ludovic and next modify by samuel*/

static void hash(lb_t *lb, pkt_t **p, uint8_t n,
                 pkt_state_t *s, uint64_t mask)
{
    for (uint8_t i = 0; i < n; i++) {
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
static uint64_t lookup(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask)
{
    (void)lb;
    (void)p;
    (void)n;
    (void)s;
    (void)mask;

    return 0;
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

static uint64_t nothing(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask)
{
    (void)lb; (void)p; (void)n; (void)s; (void)mask;
    return 0;
}

static uint32_t replica_of(const pkt_state_t *s)
{
    return s->replica;
}

static const lb_ops_t ops = {
    .state_size = sizeof(pkt_state_t),
    .init = init,
    .on_event = on_event,
    .hash = hash,
    .lookup = lookup,
    .compute_replica = compute_replica,
    .insert = nothing,
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
        /* README version */
        uint8_t n = gen_rx_batch(g, GEN_NIC_EXT, p, 64);
        uint64_t all = n == 64 ? ~0ull : (1ull << n) - 1;
        gen_hash(g, lb, p, n, s, all);
        uint64_t ok = 0;
        for (uint8_t i = 0; i < n; i++) {
            if (s[i].valid) {
                ok |= (1ull << i);
            }
        }
        uint64_t hit = gen_lookup(g, lb, p, n, s, ok);
        uint64_t got = gen_replica(g, lb, p, n, s, ok & ~hit);
        gen_insert(g, lb, p, n, s, got); /* the new flow among them */
        /* encapsulate hit | got, then: */
        gen_tx_batch(g, GEN_NIC_INT, p, n, hit | got);
        gen_drop(g, p, n, all & ~(hit | got));

        /* Stub version */
        for (int nic = GEN_NIC_EXT; nic <= GEN_NIC_INT; nic++) {
            uint8_t n = gen_rx_batch(g, nic, p, 64);
            gen_drop(g, p, n, ~0ull);
        }
    }
    gen_close(g);
    return 0;
}

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
#include "maglev.h"
#include "crc32c.h"

struct lb {
    gen_config_t cfg; // copy of gen_config_t
    mg_service_t *services;         /* one Maglev table per cfg.services[k] */
    // TODO: define flow table, hash table
};
struct pkt_state {
    flow_key_t key;
    uint32_t hash;
    uint32_t replica;
    uint32_t service_id;
    uint8_t tcp_flags;
    uint8_t valid;
};

static int is_prime(uint64_t m)
{
    if (m < 2) return 0;
    for (uint64_t d = 2; d * d <= m; d++)
        if (m % d == 0) return 0;
    return 1;
}

static lb_t *init(gen_t *g, const gen_config_t *cfg)
{
    const char *param = gen_param(g, "maglev_size");
    uint64_t size = param ? strtoull(param, NULL, 10) : 0;
    if (size > UINT32_MAX || !is_prime(size)) {
        fprintf(stderr, "maglev_size must be a prime below 2^32\n");
        return NULL;
    }

    lb_t *lb = gen_alloc(g, sizeof(lb_t), 0, 0);
    if (!lb) {
        fprintf(stderr, "gen_alloc for LB failed\n");
        return NULL;
    }
    lb->cfg = *cfg;
    lb->services = gen_alloc(g, (cfg->n_services ? cfg->n_services : 1) * sizeof *lb->services, 0, 0);
    if (!lb->services) {
        fprintf(stderr, "gen_alloc for services failed\n");
        gen_mfree(g, lb);
        return NULL;
    }
    for (uint32_t k = 0; k < cfg->n_services; k++) {
        if (!mg_init(g, &lb->services[k], &cfg->services[k], (uint32_t)size, cfg->seed)) {
            fprintf(stderr, "gen_alloc for maglev table failed\n");
            return NULL;   /* the harness frees what was allocated */
        }
    }
    return lb;
}

/*Made By ludovick and next modify by samuel*/

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
        // Compute CRC32C hash of the 5-tuple
        uint64_t ips = ((uint64_t)key->src_ip << 32) | key->dst_ip;
        uint32_t ports = ((uint32_t)key->src_port << 16) | key->dst_port;

        uint32_t h = crc32c_u64(~0u, lb->cfg.seed);
        h = crc32c_u64(h, ips);
        h = crc32c_u32(h, ports);
        h = crc32c_u8(h, key->protocol);

        s[i].hash = h;
    }
}


static uint64_t lookup(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask)
{
    (void)lb;
    (void)p;
    (void)n;
    (void)s;
    (void)mask;

    return 0;
}

static uint64_t compute_replica(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask)
{
    (void)p;
    uint64_t ok = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (!(mask & (1ull << i)) || !s[i].valid)
            continue;
        uint32_t ip = mg_lookup(&lb->services[s[i].service_id], s[i].hash);
        if (!ip)
            continue;   /* no active replica */
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

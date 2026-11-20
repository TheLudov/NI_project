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
#include <string.h>

#include "gen.h"
#include "parser.h"
#include "crc32c.h"

struct lb {
    gen_config_t cfg; // copy of gen_config_t
    // TODO: define flow table, hash table
};
struct pkt_state {
    uint32_t hash; // flow hash
    uint32_t replica; // replica chose by `compute_replica`
};

static lb_t *init(gen_t *g, const gen_config_t *cfg)
{
    lb_t *lb = gen_alloc(g, sizeof(lb_t), 0, 0);
    if (!lb) {
        fprintf(stderr, "gen_alloc for LB failed\n");
        return NULL;
    }
    lb->cfg = *cfg;
    // (void)cfg;
    // return gen_alloc(g, sizeof(lb_t), 0, 0);
    return lb;
}

static void hash(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask)
{
    /* Parse the packets*/
    for (int i = 0; i < n; i++) {
        if (!(mask & (1ull << i))) {
            continue;
        }
        const uint8_t *f = p[i]->buf + p[i]->off;
        uint16_t len = p[i]->len;
        if (len <= 47) {
            // Not a service
            return;
        }

        uint16_t e_type = rd16(f + 12);
        uint8_t version = f[14];
        if (e_type != 0x0800) {
            // Not a service
            return;
        }

        uint8_t protocol = f[23];
        if (protocol != 6) {
            // Not a service
            return;
        }
        uint32_t s_ip = rd32(f + 26);
        uint32_t d_ip = rd32(f + 30);
        uint16_t s_port = rd16(f + 34);
        uint16_t d_port = rd16(f+36);
        uint8_t flag = f[47];

        if (e_type == 0x08 && version == 0x45) {
            uint64_t ips = (uint64_t)s_ip << 32 | d_ip;
            uint64_t ports = (uint32_t)s_port << 16 | d_port;
            uint32_t h = crc32c_u64(~0u, lb->cfg.seed);
            h = crc32c_u64(h, ips);
            h = crc32c_u32(h, ports);
            h = crc32c_u8(h, protocol);

            // Find packet services
            for (uint32_t y = 0; y < lb->cfg.n_services; y++) {
                service_t service = lb->cfg.services[y];
                if (service.proto == protocol && service.vip == d_ip && service.port == d_port) {
                    // Hash their flow
                    s[i].hash = h;
                }
            }
        }
    }
}

static void lookup(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask)
{
    (void)lb; (void)p; (void)n; (void)s; (void)mask;
}

static uint64_t nothing(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask)
{
    (void)lb; (void)p; (void)n; (void)s; (void)mask;
    return 0;
}

static uint32_t replica_of(const pkt_state_t *s)
{
    (void)s;
    return 0;
}

static const lb_ops_t ops = {
    .state_size = sizeof(pkt_state_t),
    .init = init,
    .hash = hash,
    .lookup = lookup,
    .compute_replica = nothing,
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
        uint64_t ok = ...; /* the packets of a service; the other are dropped */
        uint64_t hit = gen_lookup(g, lb, n, s, ok);
        uint64_t got = gen_replica(g, lb, n, s, ok & ~hit);
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

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

#include "gen.h"

struct lb { int unused; };
struct pkt_state { int unused; };

static lb_t *init(gen_t *g, const gen_config_t *cfg)
{
    (void)cfg;
    return gen_alloc(g, sizeof(lb_t), 0, 0);
}

static void hash(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask)
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
    .lookup = nothing,
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
    pkt_t *p[64];
    while (gen_running(g)) {
        for (int nic = GEN_NIC_EXT; nic <= GEN_NIC_INT; nic++) {
            uint8_t n = gen_rx_batch(g, nic, p, 64);
            gen_drop(g, p, n, ~0ull);
        }
    }
    gen_close(g);
    return 0;
}

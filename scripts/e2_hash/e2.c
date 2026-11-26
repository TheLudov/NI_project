/* scripts/e2_hash/e2.c: E2, the hash function. Simple tabulation (tabhash_flow) against CRC32C (crc_flow).
 *
 *   e2 speed  [nb bs depth budget]    time per hash
 *   e2 spread [nb bs depth budget]    how evenly keys fill the buckets
 *   e2 fill   [nb bs depth budget]    the load factor at the first failed insert (E1's measure)
 *   e2 one    tab|crc|none            the hash test of speed for one hash function, seed 1: for perf stat, which
 *                                     counts a whole process (instructions, cache misses, ... of one hash function)
 *
 * nb bs depth budget: the table layout (the autotuner's, passed by scripts/e2_hash.sh); 65536 10 5 64 if absent.
 * Each command prints CSV on stdout, a header line first. Every measurement is made with seeds 1 to 5.
 *
 * Built by scripts/e2_hash.sh with the Makefile's flags and linked with src/: it calls the very functions the LB
 * calls (tabhash_flow, crc_flow, ft_locate, ft_insert_at), so it measures what the LB runs.
 *
 * Author: Ludovic. Measures src/tabhash.c (Ludovic, Samuel).
 */
#define _POSIX_C_SOURCE 200809L         /* for clock_gettime */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gen.h"
#include "flow_table.h"
#include "tabhash.h"

#define NSEEDS 5                        /* seeds 1..5: every point is measured 5 times (the brief's minimum) */
#define SPEED_KEYS (1u << 16)           /* speed: 65,536 keys (1 MB), read in order: the prefetcher brings them in ahead */
#define SPEED_HASHES (100u << 20)       /* speed: about 100 million hashes per measurement (a few hundred ms) */

/* the table layout (argv[2..5]) */
static uint32_t nb = 65536, bs = 10, depth = 5, budget = 64;

static void *need(void *p)              /* stops the program if an allocation failed */
{
    if (!p) {
        fprintf(stderr, "e2: out of memory (or over the 32 MB budget)\n");
        exit(1);
    }
    return p;
}

/* ---- a do-nothing LB. ft_init allocates the table with gen_alloc, which needs the gen_t that only gen_open gives,
   and gen_open needs an LB: this one. Same trick as autotune/main.c. ---- */

struct lb { int unused; };
struct pkt_state { int unused; };

static lb_t *stub_init(gen_t *g, const gen_config_t *cfg) { (void)cfg; return gen_alloc(g, sizeof(lb_t), 0, 0); }
static void stub_event(lb_t *lb, const gen_event_t *ev) { (void)lb; (void)ev; }
static void stub_hash(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t m)
{ (void)lb; (void)p; (void)n; (void)s; (void)m; }
static uint64_t stub_stage(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t m)
{ (void)lb; (void)p; (void)n; (void)s; (void)m; return 0; }
static uint32_t stub_replica_of(const pkt_state_t *s) { (void)s; return 0; }

static const lb_ops_t stub_ops = {
    .state_size = sizeof(pkt_state_t), .init = stub_init, .on_event = stub_event, .hash = stub_hash,
    .lookup = stub_stage, .compute_replica = stub_stage, .insert = stub_stage, .replica_of = stub_replica_of,
};

/* ---- the key sets: n TCP flows towards the chat VIP, 198.51.100.10:443 (host byte order, as src/parser.c) ---- */

enum { D_UNIFORM, D_SEQPORTS, D_NAT, D_N };
static const char *const DIST_NAMES[D_N] = { "uniform", "seqports", "nat" };

static uint64_t rng_state;
static uint64_t rnd64(void)             /* splitmix64, the generator tabhash_init uses */
{
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static void make_keys(flow_key_t *k, uint32_t n, int dist, uint64_t seed)
{
    uint32_t ip = 0, port = 0;
    rng_state = seed;
    for (uint32_t i = 0; i < n; i++) {
        if (dist == D_UNIFORM) {        /* every flow a new client: random address, random port */
            ip = (uint32_t)rnd64();
            port = 1024 + (uint32_t)(rnd64() % 64512);
        } else if (dist == D_SEQPORTS) {/* each client opens 16 connections, on consecutive ports */
            if (i % 16 == 0) {
                ip = (uint32_t)rnd64();
                port = 1024 + (uint32_t)(rnd64() % (64512 - 16));
            } else {
                port++;
            }
        } else {                        /* NAT: 203.0.113.0 uses ports 1024..65535, then 203.0.113.1, ...
                                           (the same keys for every seed: only the hash's seed changes) */
            ip = 0xCB007100u + i / 64512;
            port = 1024 + i % 64512;
        }
        k[i] = (flow_key_t){ .src_ip = ip, .dst_ip = 0xC6336410u, .src_port = (uint16_t)port,
                             .dst_port = 443, .protocol = 6 };
    }
}

/* The hash's secret seed in a run (the LB gets it in cfg->seed). Any value works; it differs from the keys' seed so
   that the keys and the tabulation tables come from different random streams. */
static uint64_t hash_seed(uint64_t seed) { return seed + 1000; }

/* ---- the flow table, with the given layout ---- */

/* no entry ever expires: the time stays 0 and every timeout is as long as it can be */
static const svc_timeouts_t NEVER_EXPIRE[1] = { { UINT64_MAX, UINT64_MAX, UINT64_MAX } };

static flow_table_t *table_new(gen_t *g, int hf, const tabhash_t *th, uint64_t hseed)
{
    return need(ft_init(g, nb, bs, depth, budget, 0, (uint8_t)hf, th, hseed, NEVER_EXPIRE));
}

static void table_free(gen_t *g, flow_table_t *ft)
{
    gen_mfree(g, ft->slots);
    gen_mfree(g, ft->entries);
    gen_mfree(g, ft);
}

/* ---- speed: ns per hash ---- */

enum { HF_NONE = 2 };                   /* after HASH_TAB (0) and HASH_CRC (1): no hash at all, the loop alone */
static const char *const HF_NAMES[3] = { "tab", "crc", "none" };
enum { T_HASH, T_LOCATE, T_N };
static const char *const TEST_NAMES[T_N] = { "hash", "locate" };

static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static volatile uint64_t g_sink;        /* the loop's result goes here, so that the compiler cannot delete the loop */

/* Hashes the n keys, passes times over; returns the ns per hash. hf and test do not change during a call, so the
   CPU always predicts their branches right, and they cost the same for every hash function. */
static double time_hashes(int hf, int test, const flow_key_t *keys, uint32_t n, uint32_t passes,
                          const tabhash_t *th, uint64_t hseed, const flow_table_t *ft)
{
    uint64_t sink = 0;
    double t0 = now_ns();
    for (uint32_t p = 0; p < passes; p++) {
        for (uint32_t i = 0; i < n; i++) {
            const flow_key_t *k = &keys[i];
            uint64_t h;
            if (hf == HASH_TAB)
                h = tabhash_flow(th, k);
            else if (hf == HASH_CRC)
                h = crc_flow(hseed, k);
            else
                h = k->src_ip ^ k->dst_ip ^ k->src_port;     /* none: read the key, no hash */
            if (test == T_LOCATE) {                             /* also both bucket indices and the signature */
                ft_loc_t loc;
                ft_locate(ft, h, &loc);
                h = loc.bucket[0] ^ loc.bucket[1] ^ loc.sig;
            }
            sink += h;
        }
    }
    double t = now_ns() - t0;
    g_sink = sink;
    return t / ((double)passes * n);
}

/* Tests: hash (the hash alone) and locate (the hash, then ft_locate: CRC gives 32 bits only, so ft_locate pays one
   more CRC for the second bucket). "none" measures the loop's own cost, which e2_plot.py subtracts. */
static void cmd_speed(gen_t *g)
{
    flow_key_t *keys = need(malloc(SPEED_KEYS * sizeof *keys));
    tabhash_t *th = need(gen_alloc(g, sizeof *th, 64, 0));
    /* one table, switched from one hash function to the other: ft_locate reads only its layout and its hashfn */
    flow_table_t *ft = table_new(g, HASH_TAB, th, hash_seed(1));

    printf("test,hashfn,rep,ns_per_hash\n");
    for (uint32_t rep = 0; rep < NSEEDS; rep++) {
        uint64_t hseed = hash_seed(rep + 1);                    /* repetition r uses seed r + 1 */
        make_keys(keys, SPEED_KEYS, D_UNIFORM, rep + 1);
        tabhash_init(th, hseed);
        for (int t = 0; t < T_N; t++) {
            for (int j = 0; j < 3; j++) {
                int hf = (j + (int)rep) % 3;    /* alternate the order: tab crc none, then crc none tab, ... */
                if (t == T_LOCATE && hf == HF_NONE)
                    continue;                   /* the loop's cost is measured once, in the hash test */
                ft->hashfn = hf == HASH_CRC ? HASH_CRC : HASH_TAB;
                time_hashes(hf, t, keys, SPEED_KEYS, 1, th, hseed, ft);            /* warm-up: into the cache */
                double ns = time_hashes(hf, t, keys, SPEED_KEYS, SPEED_HASHES / SPEED_KEYS, th, hseed, ft);
                printf("%s,%s,%u,%.4f\n", TEST_NAMES[t], HF_NAMES[hf], rep, ns);
                fflush(stdout);
            }
        }
    }
    table_free(g, ft);
    gen_mfree(g, th);
    free(keys);
}

/* The hash test of speed, for one hash function only (seed 1): prints the ns per hash and the number of hashes.
   perf stat runs it once per hash function; the setup (gen_open, 65,536 keys, the tables) is under 1 % of the
   counts, and "none" measures it with the loop, to subtract. */
static void cmd_one(gen_t *g, int hf)
{
    flow_key_t *keys = need(malloc(SPEED_KEYS * sizeof *keys));
    tabhash_t *th = need(gen_alloc(g, sizeof *th, 64, 0));
    uint32_t passes = SPEED_HASHES / SPEED_KEYS;
    make_keys(keys, SPEED_KEYS, D_UNIFORM, 1);
    tabhash_init(th, hash_seed(1));
    time_hashes(hf, T_HASH, keys, SPEED_KEYS, 1, th, hash_seed(1), NULL);                   /* warm-up */
    double ns = time_hashes(hf, T_HASH, keys, SPEED_KEYS, passes, th, hash_seed(1), NULL);
    printf("hashfn,hashes,ns_per_hash\n%s,%llu,%.4f\n", HF_NAMES[hf],
           (unsigned long long)(passes + 1) * SPEED_KEYS, ns);                              /* + 1: the warm-up */
    gen_mfree(g, th);
    free(keys);
}

/* ---- spread: how evenly the keys fill the buckets ---- */

/* One key per slot (n = nb x bs), so a bucket is the first bucket of bs keys on average. Counts the keys of each
   first bucket (the table's own mapping, ft_locate, so exactly the LB's buckets), then:
     z: the chi-square test against a uniform spread. chi2 = sum over buckets of (count - bs)^2 / bs is about nb - 1
        for a random hash, with a standard deviation of sqrt(2 (nb - 1)); z is the distance from nb - 1 in standard
        deviations: |z| < 2 looks random, z >> 0 means overloaded buckets, z << 0 a spread too even to be random;
     fullest: the largest count. Beyond bs, the bucket's extra keys must go to their second bucket. */
static void cmd_spread(gen_t *g)
{
    uint32_t n = nb * bs;
    flow_key_t *keys = need(malloc((size_t)n * sizeof *keys));
    uint32_t *count = need(malloc((size_t)nb * sizeof *count));
    tabhash_t *th = need(gen_alloc(g, sizeof *th, 64, 0));

    printf("hashfn,dist,seed,keys,buckets,bucket_size,z,fullest\n");
    for (uint64_t seed = 1; seed <= NSEEDS; seed++) {
        uint64_t hseed = hash_seed(seed);
        tabhash_init(th, hseed);
        for (int dist = 0; dist < D_N; dist++) {
            make_keys(keys, n, dist, seed);
            for (int hf = HASH_TAB; hf <= HASH_CRC; hf++) {
                flow_table_t *ft = table_new(g, hf, th, hseed);
                memset(count, 0, (size_t)nb * sizeof *count);
                for (uint32_t i = 0; i < n; i++) {
                    ft_loc_t loc;
                    ft_locate(ft, compute_hash((uint8_t)hf, th, hseed, &keys[i]), &loc);
                    count[loc.bucket[0]]++;
                }
                double mean = (double)n / nb, chi2 = 0;
                uint32_t fullest = 0;
                for (uint32_t b = 0; b < nb; b++) {
                    double d = count[b] - mean;
                    chi2 += d * d / mean;
                    if (count[b] > fullest)
                        fullest = count[b];
                }
                double z = (chi2 - (nb - 1)) / sqrt(2.0 * (nb - 1));
                printf("%s,%s,%llu,%u,%u,%u,%.2f,%u\n", HF_NAMES[hf], DIST_NAMES[dist], (unsigned long long)seed,
                       n, nb, bs, z, fullest);
                fflush(stdout);
                table_free(g, ft);
            }
        }
    }
    gen_mfree(g, th);
    free(count);
    free(keys);
}

/* ---- fill: the load factor at the first failed insert ---- */

/* Inserts keys one by one, as the LB does (ft_locate, then ft_insert_at), until an insert fails. 10 % more keys
   than slots, so the table always fails before the keys run out. load = keys stored / slots. */
static void cmd_fill(gen_t *g)
{
    uint32_t cap = nb * bs, n = cap + cap / 10;
    flow_key_t *keys = need(malloc((size_t)n * sizeof *keys));
    tabhash_t *th = need(gen_alloc(g, sizeof *th, 64, 0));

    printf("hashfn,dist,seed,buckets,bucket_size,max_depth,dfs_budget,stored,load_at_failure\n");
    for (uint64_t seed = 1; seed <= NSEEDS; seed++) {
        uint64_t hseed = hash_seed(seed);
        tabhash_init(th, hseed);
        for (int dist = 0; dist < D_N; dist++) {
            make_keys(keys, n, dist, seed);
            for (int hf = HASH_TAB; hf <= HASH_CRC; hf++) {
                flow_table_t *ft = table_new(g, hf, th, hseed);
                uint32_t stored = 0;
                while (stored < n) {
                    ft_loc_t loc;
                    ft_locate(ft, compute_hash((uint8_t)hf, th, hseed, &keys[stored]), &loc);
                    if (!ft_insert_at(ft, &keys[stored], 0, FS_ESTABLISHED, &loc, 0, 0))
                        break;                                  /* the first failed insert */
                    stored++;
                }
                printf("%s,%s,%llu,%u,%u,%u,%u,%u,%.4f\n", HF_NAMES[hf], DIST_NAMES[dist],
                       (unsigned long long)seed, nb, bs, depth, budget, stored, (double)stored / cap);
                fflush(stdout);
                table_free(g, ft);
            }
        }
    }
    gen_mfree(g, th);
    free(keys);
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "";
    int one = -1;                       /* e2 one HF: the hash function */
    if (!strcmp(cmd, "one") && argc == 3)
        for (int hf = 0; hf < 3; hf++)
            if (!strcmp(argv[2], HF_NAMES[hf]))
                one = hf;
    int known = !strcmp(cmd, "speed") || !strcmp(cmd, "spread") || !strcmp(cmd, "fill");
    if (one < 0 && (!known || (argc != 2 && argc != 6))) {
        fprintf(stderr, "usage: %s speed|spread|fill [nb bs depth budget]\n"
                        "       %s one tab|crc|none\n", argv[0], argv[0]);
        return 2;
    }
    if (known && argc == 6) {
        nb = (uint32_t)strtoul(argv[2], NULL, 10);
        bs = (uint32_t)strtoul(argv[3], NULL, 10);
        depth = (uint32_t)strtoul(argv[4], NULL, 10);
        budget = (uint32_t)strtoul(argv[5], NULL, 10);
    }
    gen_t *g = need(gen_open(NULL, "mode=bench,results=/dev/null,pool_mb=8,bench_keys=1k", &stub_ops));
    if (one >= 0)
        cmd_one(g, one);
    else if (!strcmp(cmd, "speed"))
        cmd_speed(g);
    else if (!strcmp(cmd, "spread"))
        cmd_spread(g);
    else
        cmd_fill(g);
    gen_close(g);
    return 0;
}

/* scripts/e2_hash/e2.c: E2, the hash function: simple tabulation (tabhash_flow) against CRC32C (crc_flow).
 *
 * Not part of the LB: built by scripts/e2_hash.sh with the Makefile's flags plus -DFT_STATS=1, linked with src/ and
 * the harness (only for gen_alloc, through a stub LB opened in bench mode, as autotune/main.c does). It calls the very
 * functions the LB calls (tabhash_flow, crc_flow, ft_locate, ft_insert_at), so it measures what the LB runs.
 *
 *   e2 speed  [keys=1M] [hashes=20M] [reps=5] [K=0,1,2,4,8] [pollute_kb=256] [nb=65536] [bs=10]
 *   e2 spread [n=760k] [nb=65536] [bs=10] [seeds=1,2,3,4,5] [dists=...] [flowlog=FILE]
 *   e2 fill   [nb=65536] [bs=10] [depth=5] [budget=64] [seeds=1,2,3,4,5] [dists=...] [flowlog=FILE]
 *
 * Each prints CSV on stdout (a header first).
 *   speed: ns per hash, for tab, crc and "none" (a few XORs: the loop's own cost, to subtract), in three tests:
 *     thr     independent hashes, one after the other (what a batch is);
 *     lat     each key depends on the previous hash (src_port ^= hash): the hash's latency;
 *     locate  hash then ft_locate (both bucket indices and the signature: CRC pays a 5th CRC there).
 *     With K > 0, every hash also reads K random cache lines of a pollute_kb buffer (default 256 KB: more than L1,
 *     less than L2), the stand-ins for the packet headers, buckets and entries the LB reads per packet: they evict
 *     the tabulation tables from L1. Tests and hash functions alternate within each repetition.
 *   spread: how evenly keys fill the buckets of a table of nb buckets (the table's own mapping, ft_locate): per
 *     bucket counts of the first and second bucket (chi-square against uniform: about nb - 1 if random; z is its
 *     distance in standard deviations), the fullest bucket, empty buckets against Poisson, the chi-square of the
 *     signature, the collisions of the 32 bits CRC gives (against n^2 / 2^33) and the keys that share buckets AND
 *     signature (the index cannot tell them apart before reading the entry array).
 *   fill: inserts until the first failure: load factor then, and displacements per insert by load (bins of 5 %).
 *
 * Key sets (dists), n distinct 5-tuples towards the scenarios' three VIPs, port 443, TCP:
 *   uniform   random client address, random ephemeral port;
 *   seqports  clients opening 16 connections each on consecutive source ports (random address and start);
 *   nat       as few NAT addresses as possible (203.0.113.x), each handing out ports 1024, 1025, ... to one VIP;
 *   mixed     the autotuner's mix (autotune/main.c make_keys): 70 % own address, 30 % behind 64 NAT addresses;
 *   flowlog   the first n flows of a harness flowlog= file (real 5-tuples of a scenario).
 *
 * Author: Ludovic. Measures src/tabhash.c (Ludovic, Samuel).
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gen.h"
#include "flow_table.h"
#include "tabhash.h"

/* ---- a minimal LB, only so that gen_open gives us the harness's allocator (as autotune/main.c) ---- */

struct lb { int unused; };
struct pkt_state { int unused; };

static lb_t *stub_init(gen_t *g, const gen_config_t *cfg)
{
    (void)cfg;
    lb_t *lb = gen_alloc(g, sizeof *lb, 0, 0);
    if (!lb) {
        fprintf(stderr, "e2: gen_alloc failed in init\n");
        exit(1);
    }
    return lb;
}
static void stub_on_event(lb_t *lb, const gen_event_t *ev) { (void)lb; (void)ev; }
static void stub_hash(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t m)
{ (void)lb; (void)p; (void)n; (void)s; (void)m; }
static uint64_t stub_stage(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t m)
{ (void)lb; (void)p; (void)n; (void)s; (void)m; return 0; }
static uint32_t stub_replica_of(const pkt_state_t *s) { (void)s; return 0; }

static const lb_ops_t stub_ops = {
    .state_size = sizeof(pkt_state_t),
    .init = stub_init,
    .on_event = stub_on_event,
    .hash = stub_hash,
    .lookup = stub_stage,
    .compute_replica = stub_stage,
    .insert = stub_stage,
    .replica_of = stub_replica_of,
};

/* ---- arguments: key=value ---- */

static int g_argc;
static char **g_argv;

static const char *arg_str(const char *key, const char *def)
{
    size_t l = strlen(key);
    for (int i = 2; i < g_argc; i++)
        if (strncmp(g_argv[i], key, l) == 0 && g_argv[i][l] == '=')
            return g_argv[i] + l + 1;
    return def;
}

/* a whole number with an optional k or M suffix */
static uint64_t parse_num(const char *v)
{
    char *end;
    double x = strtod(v, &end);
    if (*end == 'k' || *end == 'K')
        x *= 1e3;
    else if (*end == 'M')
        x *= 1e6;
    return (uint64_t)x;
}

static uint64_t arg_num(const char *key, const char *def) { return parse_num(arg_str(key, def)); }

/* the next item of a comma-separated list into buf; returns the rest, or NULL at the end */
static const char *next_item(const char *list, char *buf, size_t cap)
{
    if (!list || !*list)
        return NULL;
    size_t l = strcspn(list, ",");
    if (l >= cap)
        l = cap - 1;
    memcpy(buf, list, l);
    buf[l] = '\0';
    return list[l] == ',' ? list + l + 1 : list + l;
}

/* ---- keys ---- */

enum { D_UNIFORM, D_SEQPORTS, D_NAT, D_MIXED, D_FLOWLOG, D_N };
static const char *const DIST_NAMES[D_N] = { "uniform", "seqports", "nat", "mixed", "flowlog" };

static int dist_of(const char *name)
{
    for (int d = 0; d < D_N; d++)
        if (strcmp(name, DIST_NAMES[d]) == 0)
            return d;
    fprintf(stderr, "e2: unknown dist %s\n", name);
    exit(2);
}

static uint64_t rng_state;
static uint64_t rnd64(void)        /* splitmix64 */
{
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static const uint32_t VIP[3] = { 0xC6336410u, 0xC6336414u, 0xC633641Eu };   /* 198.51.100.10/.20/.30 */

static uint32_t pick_vip(uint64_t r)       /* the scenarios' mix: chat holds almost all flows */
{
    uint32_t p = (uint32_t)(r % 1000);
    return VIP[p < 960 ? 0 : p < 965 ? 1 : 2];
}

static void tcp_key(flow_key_t *k, uint32_t sip, uint16_t sport, uint32_t dip)
{
    memset(k, 0, sizeof *k);
    k->src_ip = sip;
    k->src_port = sport;
    k->dst_ip = dip;
    k->dst_port = 443;
    k->protocol = 6;
}

/* Reads up to n flows of a harness flowlog file (saddr,daddr,sport,dport,proto,...); returns how many. */
static uint32_t read_flowlog(const char *path, flow_key_t *k, uint32_t n)
{
    FILE *f = path ? fopen(path, "r") : NULL;
    if (!f) {
        fprintf(stderr, "e2: dist flowlog needs flowlog=FILE (a harness flowlog= output)\n");
        exit(2);
    }
    char line[512];
    uint32_t got = 0;
    while (got < n && fgets(line, sizeof line, f)) {
        unsigned a[4], b[4], sp, dp, pr;
        if (sscanf(line, "%u.%u.%u.%u,%u.%u.%u.%u,%u,%u,%u", &a[0], &a[1], &a[2], &a[3],
                   &b[0], &b[1], &b[2], &b[3], &sp, &dp, &pr) != 11)
            continue;                                  /* the header */
        flow_key_t *x = &k[got++];
        memset(x, 0, sizeof *x);
        x->src_ip = a[0] << 24 | a[1] << 16 | a[2] << 8 | a[3];
        x->dst_ip = b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
        x->src_port = (uint16_t)sp;
        x->dst_port = (uint16_t)dp;
        x->protocol = (uint8_t)pr;
    }
    fclose(f);
    return got;
}

/* Fills k with n keys of the given set; returns how many (fewer only for a short flowlog). Host byte order, as the
   LB's parser leaves them (src/parser.c). */
static uint32_t make_keys(flow_key_t *k, uint32_t n, int dist, uint64_t seed, const char *flowlog)
{
    rng_state = seed * 0x2545F4914F6CDD1Dull + 1;
    uint32_t ip = 0, start = 0, vip = VIP[0], nat = 0;
    switch (dist) {
    case D_UNIFORM:
        for (uint32_t i = 0; i < n; i++) {
            uint64_t r = rnd64();
            tcp_key(&k[i], (uint32_t)(r >> 32), (uint16_t)(1024 + (uint32_t)r % 64512), pick_vip(rnd64()));
        }
        return n;
    case D_SEQPORTS:
        for (uint32_t i = 0; i < n; i++) {
            if (i % 16 == 0) {
                uint64_t r = rnd64();
                ip = (uint32_t)(r >> 32);
                start = 1024 + (uint32_t)r % (64512 - 16);
                vip = pick_vip(rnd64());
            }
            tcp_key(&k[i], ip, (uint16_t)(start + i % 16), vip);
        }
        return n;
    case D_NAT:
        for (uint32_t i = 0; i < n; i++)
            tcp_key(&k[i], 0xCB007100u + i / 64512, (uint16_t)(1024 + i % 64512), VIP[0]);
        return n;
    case D_MIXED:                                          /* autotune/main.c make_keys */
        for (uint32_t i = 0; i < n; i++) {
            uint64_t r = rnd64();
            uint32_t pick = (uint32_t)(r % 1000);
            uint32_t d = VIP[pick < 960 ? 0 : pick < 965 ? 1 : 2];
            if ((r >> 10) % 10 < 7) {
                tcp_key(&k[i], (uint32_t)(r >> 32), (uint16_t)(1024 + (rnd64() % 64512)), d);
            } else {
                uint32_t a = nat % 64, j = nat / 64;
                tcp_key(&k[i], 0xCB007100u + a + 64u * (j / 64512), (uint16_t)(1024 + j % 64512), d);
                nat++;
            }
        }
        return n;
    default:
        return read_flowlog(flowlog, k, n);
    }
}

/* ---- hashing as the LB does it ---- */

enum { HF_TAB = HASH_TAB, HF_CRC = HASH_CRC, HF_NONE };
static const char *const HF_NAMES[3] = { "tab", "crc", "none" };

static int hf_of(const char *name)
{
    for (int h = 0; h < 3; h++)
        if (strcmp(name, HF_NAMES[h]) == 0)
            return h;
    fprintf(stderr, "e2: unknown hashfn %s\n", name);
    exit(2);
}

/* The hash seed of a run (the LB's cfg->seed): anything not 0 */
static uint64_t hash_seed(uint64_t seed) { return seed * 0x9E3779B97F4A7C15ull + 0x1234567ull; }

/* The tuner's flows never expire: every timeout as long as it can be, the time stays 0 (one service, 0) */
static const svc_timeouts_t NEVER_EXPIRE[1] = { { UINT64_MAX, UINT64_MAX, UINT64_MAX } };

static flow_table_t *table_new(gen_t *g, uint32_t nb, uint32_t bs, uint32_t depth, uint32_t budget, int hf,
                               const tabhash_t *th, uint64_t hseed)
{
    flow_table_t *ft = ft_init(g, nb, bs, depth, budget, 0, (uint8_t)hf, th, hseed, NEVER_EXPIRE);
    if (!ft) {
        fprintf(stderr, "e2: ft_init(%u buckets x %u) failed: over the memory budget?\n", nb, bs);
        exit(1);
    }
    return ft;
}

static void table_free(gen_t *g, flow_table_t *ft)
{
    gen_mfree(g, ft->slots);
    gen_mfree(g, ft->entries);
    gen_mfree(g, ft);
}

static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

/* ---- speed ---- */

enum { T_THR, T_LAT, T_LOCATE, T_N };
static const char *const TEST_NAMES[T_N] = { "thr", "lat", "locate" };

static volatile uint64_t g_sink;

/* ns per hash over passes x n keys. hf, test and K are loop invariants: the branches on them are always predicted
   the same way, and cost the same for the three hash functions (the "none" run measures them). */
static double time_loop(int hf, int test, uint32_t K, const flow_key_t *keys, uint32_t n, uint64_t passes,
                        const tabhash_t *th, uint64_t hseed, const flow_table_t *ft,
                        const uint8_t *pol, uint64_t pol_mask)
{
    uint64_t sink = 0, prev = 0, r = 0x853C49E6748FEA9Bull;
    double t0 = now_ns();
    for (uint64_t p = 0; p < passes; p++) {
        for (uint32_t i = 0; i < n; i++) {
            flow_key_t k = keys[i];
            if (test == T_LAT)
                k.src_port ^= (uint16_t)prev;
            for (uint32_t j = 0; j < K; j++) {     /* K random cache lines of the pollution buffer */
                r = r * 6364136223846793005ull + 1442695040888963407ull;
                sink += pol[((r >> 20) & pol_mask) << 6];
            }
            uint64_t h;
            if (hf == HF_TAB)
                h = tabhash_flow(th, &k);
            else if (hf == HF_CRC)
                h = crc_flow(hseed, &k);
            else
                h = (uint64_t)k.src_ip ^ ((uint64_t)k.src_port << 32) ^ k.dst_ip;
            if (test == T_LOCATE) {
                ft_loc_t loc;
                ft_locate(ft, h, &loc);
                h = loc.bucket[0] ^ ((uint64_t)loc.bucket[1] << 32) ^ loc.sig;
            }
            sink += h;
            prev = h;
        }
    }
    double t = now_ns() - t0;
    g_sink = sink;
    return t / ((double)passes * n);
}

static int cmd_speed(gen_t *g)
{
    uint32_t n = (uint32_t)arg_num("keys", "1M");
    uint64_t hashes = arg_num("hashes", "20M");
    uint32_t reps = (uint32_t)arg_num("reps", "5");
    uint32_t nb = (uint32_t)arg_num("nb", "65536"), bs = (uint32_t)arg_num("bs", "10");
    uint64_t pol_kb = arg_num("pollute_kb", "256");
    const char *klist = arg_str("K", "0,1,2,4,8");
    uint64_t passes = (hashes + n - 1) / n;

    flow_key_t *keys = malloc((size_t)n * sizeof *keys);
    /* the pollution buffer: a power of 2 of cache lines, at least pollute_kb */
    uint64_t lines = 1;
    while (lines * 64 < pol_kb * 1024)
        lines <<= 1;
    uint8_t *pol = malloc(lines * 64);
    if (!keys || !pol) {
        fprintf(stderr, "e2: malloc failed\n");
        return 1;
    }
    memset(pol, 1, lines * 64);
    make_keys(keys, n, D_MIXED, 1, NULL);

    uint64_t hseed = hash_seed(1);
    tabhash_t *th = gen_alloc(g, sizeof *th, 64, 0);
    if (!th) {
        fprintf(stderr, "e2: gen_alloc failed\n");
        return 1;
    }
    tabhash_init(th, hseed);
    /* one table for ft_locate (two would not fit the budget): its mapping depends only on hashfn, set before each
       measurement (num_buckets and sig_bits are the same for both) */
    flow_table_t *ft = table_new(g, nb, bs, 5, 64, HF_TAB, th, hseed);

    fprintf(stderr, "e2 speed: %u keys x %llu passes, %u reps, pollution %llu KB, K=%s\n", n,
            (unsigned long long)passes, reps, (unsigned long long)(lines * 64 / 1024), klist);
    printf("test,hashfn,K,pollute_kb,rep,ns_per_hash\n");
    for (uint32_t rep = 0; rep < reps; rep++) {
        char kb[32];
        for (const char *kl = next_item(klist, kb, sizeof kb); kl; kl = next_item(kl, kb, sizeof kb)) {
            uint32_t K = (uint32_t)parse_num(kb);
            for (int t = 0; t < T_N; t++) {
                for (int j = 0; j < 3; j++) {
                    int hf = (j + (int)rep) % 3;            /* alternate the order from one repetition to the next */
                    ft->hashfn = hf == HF_CRC ? HASH_CRC : HASH_TAB;
                    const flow_table_t *f = ft;
                    time_loop(hf, t, K, keys, n, 1, th, hseed, f, pol, lines - 1);          /* warm-up */
                    double ns = time_loop(hf, t, K, keys, n, passes, th, hseed, f, pol, lines - 1);
                    printf("%s,%s,%u,%llu,%u,%.4f\n", TEST_NAMES[t], HF_NAMES[hf], K,
                           (unsigned long long)(lines * 64 / 1024), rep, ns);
                    fflush(stdout);
                }
            }
        }
    }
    table_free(g, ft);
    gen_mfree(g, th);
    free(keys);
    free(pol);
    return 0;
}

/* ---- spread ---- */

typedef struct { uint32_t b1, b2, sig; } triple_t;

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static int cmp_triple(const void *a, const void *b)
{
    const triple_t *x = a, *y = b;
    if (x->b1 != y->b1) return x->b1 < y->b1 ? -1 : 1;
    if (x->b2 != y->b2) return x->b2 < y->b2 ? -1 : 1;
    return x->sig < y->sig ? -1 : x->sig > y->sig;
}

/* pairs of equal values in a sorted array: sum of c(c-1)/2 over runs */
static uint64_t equal_pairs_u32(const uint32_t *v, uint32_t n)
{
    uint64_t pairs = 0, run = 1;
    for (uint32_t i = 1; i <= n; i++) {
        if (i < n && v[i] == v[i - 1]) {
            run++;
        } else {
            pairs += run * (run - 1) / 2;
            run = 1;
        }
    }
    return pairs;
}

static uint64_t equal_pairs_triple(const triple_t *v, uint32_t n)
{
    uint64_t pairs = 0, run = 1;
    for (uint32_t i = 1; i <= n; i++) {
        if (i < n && cmp_triple(&v[i], &v[i - 1]) == 0) {
            run++;
        } else {
            pairs += run * (run - 1) / 2;
            run = 1;
        }
    }
    return pairs;
}

/* chi-square of counts against uniform; also the largest count and the empty cells */
static double chi2_of(const uint32_t *c, uint32_t cells, uint32_t n, uint32_t *max, uint32_t *empty)
{
    double lambda = (double)n / cells, chi2 = 0;
    *max = 0;
    *empty = 0;
    for (uint32_t i = 0; i < cells; i++) {
        double d = c[i] - lambda;
        chi2 += d * d / lambda;
        if (c[i] > *max)
            *max = c[i];
        if (c[i] == 0)
            (*empty)++;
    }
    return chi2;
}

/* distance of a chi-square from its mean, in standard deviations (df large: chi2 ~ N(df, 2 df)) */
static double chi2_z(double chi2, uint32_t cells) { return (chi2 - (cells - 1)) / sqrt(2.0 * (cells - 1)); }

static int cmd_spread(gen_t *g)
{
    uint32_t n = (uint32_t)arg_num("n", "760k");
    uint32_t nb = (uint32_t)arg_num("nb", "65536"), bs = (uint32_t)arg_num("bs", "10");
    const char *seeds = arg_str("seeds", "1,2,3,4,5");
    const char *dists = arg_str("dists", "uniform,seqports,nat,mixed");
    const char *hfs = arg_str("hashfns", "tab,crc");
    const char *flowlog = arg_str("flowlog", NULL);

    flow_key_t *keys = malloc((size_t)n * sizeof *keys);
    uint32_t *b1c = malloc((size_t)nb * sizeof *b1c), *b2c = malloc((size_t)nb * sizeof *b2c);
    uint32_t *h32 = malloc((size_t)n * sizeof *h32);
    triple_t *tri = malloc((size_t)n * sizeof *tri);
    tabhash_t *th = gen_alloc(g, sizeof *th, 64, 0);
    if (!keys || !b1c || !b2c || !h32 || !tri || !th) {
        fprintf(stderr, "e2: allocation failed\n");
        return 1;
    }

    printf("hashfn,dist,seed,n,nb,lambda,chi2_b1,z_b1,max_b1,empty_b1,empty_poisson,chi2_b2,z_b2,max_b2,"
           "sig_bits,sig_cells,chi2_sig,z_sig,coll32,coll32_expected,same_b1b2sig,same_b1b2sig_expected\n");
    char sb[32], db[32], hb[32];
    for (const char *sl = next_item(seeds, sb, sizeof sb); sl; sl = next_item(sl, sb, sizeof sb)) {
        uint64_t seed = parse_num(sb), hseed = hash_seed(seed);
        tabhash_init(th, hseed);
        for (const char *dl = next_item(dists, db, sizeof db); dl; dl = next_item(dl, db, sizeof db)) {
            int dist = dist_of(db);
            uint32_t m = make_keys(keys, n, dist, seed, flowlog);
            for (const char *hl = next_item(hfs, hb, sizeof hb); hl; hl = next_item(hl, hb, sizeof hb)) {
                int hf = hf_of(hb);
                flow_table_t *ft = table_new(g, nb, bs, 5, 64, hf, th, hseed);
                uint32_t sbits = ft->sig_bits < 16 ? ft->sig_bits : 16;  /* the signature's low 16 bits at most */
                uint32_t scells = 1u << sbits;
                uint32_t *sc = calloc(scells, sizeof *sc);
                if (!sc) {
                    fprintf(stderr, "e2: calloc failed\n");
                    return 1;
                }
                memset(b1c, 0, (size_t)nb * sizeof *b1c);
                memset(b2c, 0, (size_t)nb * sizeof *b2c);
                for (uint32_t i = 0; i < m; i++) {
                    uint64_t h = compute_hash((uint8_t)hf, th, hseed, &keys[i]);
                    ft_loc_t loc;
                    ft_locate(ft, h, &loc);
                    b1c[loc.bucket[0]]++;
                    b2c[loc.bucket[1]]++;
                    sc[loc.sig & (scells - 1)]++;
                    h32[i] = (uint32_t)h;
                    tri[i] = (triple_t){ loc.bucket[0], loc.bucket[1], loc.sig };
                }
                uint32_t max1, empty1, max2, empty2, smax, sempty;
                double c1 = chi2_of(b1c, nb, m, &max1, &empty1);
                double c2 = chi2_of(b2c, nb, m, &max2, &empty2);
                double cs = chi2_of(sc, scells, m, &smax, &sempty);
                qsort(h32, m, sizeof *h32, cmp_u32);
                qsort(tri, m, sizeof *tri, cmp_triple);
                uint64_t coll = equal_pairs_u32(h32, m), same = equal_pairs_triple(tri, m);
                double pairs = (double)m * (m - 1) / 2, lambda = (double)m / nb;
                printf("%s,%s,%llu,%u,%u,%.4f,%.1f,%.2f,%u,%.6f,%.6f,%.1f,%.2f,%u,%u,%u,%.1f,%.2f,%llu,%.2f,%llu,%.4f\n",
                       HF_NAMES[hf], DIST_NAMES[dist], (unsigned long long)seed, m, nb, lambda,
                       c1, chi2_z(c1, nb), max1, (double)empty1 / nb, exp(-lambda),
                       c2, chi2_z(c2, nb), max2, ft->sig_bits, scells, cs, chi2_z(cs, scells),
                       (unsigned long long)coll, pairs / 4294967296.0,
                       (unsigned long long)same, pairs / ((double)nb * (nb - 1) * ldexp(1.0, ft->sig_bits)));
                fflush(stdout);
                free(sc);
                table_free(g, ft);
            }
        }
    }
    gen_mfree(g, th);
    free(keys); free(b1c); free(b2c); free(h32); free(tri);
    return 0;
}

/* ---- fill: load factor at the first failed insert, and displacements per insert as the table fills ---- */

#define BINS 20             /* load bins of 5 % */

static int cmd_fill(gen_t *g)
{
    uint32_t nb = (uint32_t)arg_num("nb", "65536"), bs = (uint32_t)arg_num("bs", "10");
    uint32_t depth = (uint32_t)arg_num("depth", "5"), budget = (uint32_t)arg_num("budget", "64");
    const char *seeds = arg_str("seeds", "1,2,3,4,5");
    const char *dists = arg_str("dists", "uniform,seqports,nat,mixed");
    const char *hfs = arg_str("hashfns", "tab,crc");
    const char *flowlog = arg_str("flowlog", NULL);
    uint32_t cap = nb * bs, n = cap + cap / 10;     /* more keys than slots: the table must fail first */

    flow_key_t *keys = malloc((size_t)n * sizeof *keys);
    tabhash_t *th = gen_alloc(g, sizeof *th, 64, 0);
    if (!keys || !th) {
        fprintf(stderr, "e2: allocation failed\n");
        return 1;
    }
    printf("kind,hashfn,dist,seed,nb,bs,depth,budget,load_lo,inserts,displacements_per_insert,load_at_failure\n");
    char sb[32], db[32], hb[32];
    for (const char *sl = next_item(seeds, sb, sizeof sb); sl; sl = next_item(sl, sb, sizeof sb)) {
        uint64_t seed = parse_num(sb), hseed = hash_seed(seed);
        tabhash_init(th, hseed);
        for (const char *dl = next_item(dists, db, sizeof db); dl; dl = next_item(dl, db, sizeof db)) {
            int dist = dist_of(db);
            uint32_t m = make_keys(keys, n, dist, seed, flowlog);
            for (const char *hl = next_item(hfs, hb, sizeof hb); hl; hl = next_item(hl, hb, sizeof hb)) {
                int hf = hf_of(hb);
                flow_table_t *ft = table_new(g, nb, bs, depth, budget, hf, th, hseed);
                uint64_t ins[BINS] = { 0 }, disp[BINS] = { 0 };
                int failed = 0;
                for (uint32_t i = 0; i < m; i++) {
                    uint32_t bin = (uint32_t)((uint64_t)ft->stats.inserts * BINS / cap);
                    uint64_t d0 = ft->stats.displacements, i0 = ft->stats.inserts;
                    uint64_t h = compute_hash((uint8_t)hf, th, hseed, &keys[i]);
                    ft_loc_t loc;
                    ft_locate(ft, h, &loc);
                    if (!ft_insert_at(ft, &keys[i], 0, FS_ESTABLISHED, &loc, 0, 0)) {
                        failed = 1;
                        break;
                    }
                    if (bin < BINS) {
                        ins[bin] += ft->stats.inserts - i0;
                        disp[bin] += ft->stats.displacements - d0;
                    }
                }
                double lf = (double)ft->stats.inserts / cap;
                for (int b = 0; b < BINS; b++)
                    if (ins[b])
                        printf("curve,%s,%s,%llu,%u,%u,%u,%u,%.2f,%llu,%.4f,\n", HF_NAMES[hf], DIST_NAMES[dist],
                               (unsigned long long)seed, nb, bs, depth, budget, (double)b / BINS,
                               (unsigned long long)ins[b], (double)disp[b] / ins[b]);
                printf("%s,%s,%s,%llu,%u,%u,%u,%u,,%llu,%.4f,%.4f\n", failed ? "first_fail" : "no_failure",
                       HF_NAMES[hf], DIST_NAMES[dist], (unsigned long long)seed, nb, bs, depth, budget,
                       (unsigned long long)ft->stats.inserts,
                       ft->stats.inserts ? (double)ft->stats.displacements / ft->stats.inserts : 0.0, lf);
                fflush(stdout);
                table_free(g, ft);
            }
        }
    }
    gen_mfree(g, th);
    free(keys);
    return 0;
}

int main(int argc, char **argv)
{
    g_argc = argc;
    g_argv = argv;
    if (argc < 2 || (strcmp(argv[1], "speed") && strcmp(argv[1], "spread") && strcmp(argv[1], "fill"))) {
        fprintf(stderr, "usage: %s speed|spread|fill [key=value ...] (see the top of scripts/e2_hash/e2.c)\n",
                argv[0]);
        return 2;
    }
    gen_t *g = gen_open(NULL, "mode=bench,results=/dev/null,pool_mb=8,bench_keys=1k", &stub_ops);
    if (!g) {
        fprintf(stderr, "e2: gen_open failed\n");
        return 1;
    }
    int rc = strcmp(argv[1], "speed") == 0 ? cmd_speed(g) : strcmp(argv[1], "spread") == 0 ? cmd_spread(g)
                                                                                         : cmd_fill(g);
    gen_close(g);
    return rc;
}

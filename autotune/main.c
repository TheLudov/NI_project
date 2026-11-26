/* autotune/main.c: chooses the bucket layout of the flow table.
 * Author: Atieh El Gebrin
 */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gen.h"
#include "flow_table.h"
#include "tabhash.h"

/* ---- tunables of the tuner ---- */

#define RESERVE_BYTES  (2u << 20)  /* the LB's other state: rings (~1.7 MB at ring_v=1000), tabulation tables 26 KB */
#define SLOT_BYTES     (sizeof(uint32_t) + sizeof(flow_entry_t))   /* one index slot + its entry: 36 B */
#define TARGET_FLOWS   760000u     /* measured peak of live entries, 750k, rounded up (see above) */
#define MARGIN         0.05        /* usable capacity must exceed the target by 5 % (the peak is measured, not guessed) */
#define HIT_FRACTION   0.90        /* share of packets of known flows (the harness's bench default, bench_hit) */
#define TIE            0.10        /* costs within 10 % of the lowest are ties (run-to-run spread on our VM) */
#define REPS           5           /* timing runs per candidate (median) */
#define FILL_SEEDS     3           /* seeds for the usable-capacity search (the worst one counts) */
#define N_INSERT       50000u      /* inserts timed at the target load */
#define N_LOOKUP       1000000u    /* lookups timed (hits, then misses) */
#define N_OVER         50000u      /* insert attempts timed on a table filled to its first failure */

static const uint32_t BUCKET_SIZES[] = { 2, 4, 8, 10, 16 };
static const uint32_t DEPTHS[]       = { 2, 4, 8 };
static const uint32_t BUDGETS[]      = { 16, 64, 256 };
#define LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ---- a minimal LB, only so that gen_open gives us the harness's allocator and memory budget ---- */

struct lb { size_t mem_budget; };
struct pkt_state { int unused; };

static lb_t *bench_init(gen_t *g, const gen_config_t *cfg)
{
    lb_t *lb = gen_alloc(g, sizeof *lb, 0, 0);
    if (!lb) {
        fprintf(stderr, "autotune: gen_alloc failed in init\n");
        exit(1);
    }
    lb->mem_budget = cfg->mem_budget;
    return lb;
}
static void bench_on_event(lb_t *lb, const gen_event_t *ev) { (void)lb; (void)ev; }
static void bench_hash(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t m)
{ (void)lb; (void)p; (void)n; (void)s; (void)m; }
static uint64_t bench_stage(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t m)
{ (void)lb; (void)p; (void)n; (void)s; (void)m; return 0; }
static uint32_t bench_replica_of(const pkt_state_t *s) { (void)s; return 0; }

static const lb_ops_t bench_ops = {
    .state_size = sizeof(pkt_state_t),
    .init = bench_init,
    .on_event = bench_on_event,
    .hash = bench_hash,
    .lookup = bench_stage,
    .compute_replica = bench_stage,
    .insert = bench_stage,
    .replica_of = bench_replica_of,
};

/* ---- keys: realistic 5-tuples ---- */

typedef struct { flow_key_t key; uint64_t hash; } tkey_t;

static uint64_t rng_state;
static uint64_t rnd64(void)        /* splitmix64 */
{
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* The three services of the scenarios; chat holds almost all flows. 70 % of the clients have their own address and a
   random ephemeral port; 30 % sit behind 64 NAT addresses with sequential source ports (E2's hard case). */
static void make_keys(tkey_t *k, uint32_t n, uint64_t seed, const tabhash_t *th)
{
    static const uint32_t vip[3] = { 0xC6336410u, 0xC6336414u, 0xC633641Eu };  /* 198.51.100.10/.20/.30 */
    uint32_t nat = 0;                                        /* NAT clients so far */
    rng_state = seed;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t r = rnd64();
        uint32_t pick = (uint32_t)(r % 1000);
        int svc = pick < 960 ? 0 : pick < 965 ? 1 : 2;       /* ~ 625k : 3k : 22k */
        flow_key_t *f = &k[i].key;
        memset(f, 0, sizeof *f);
        f->dst_ip = vip[svc];
        f->dst_port = 443;
        f->protocol = 6;
        if ((r >> 10) % 10 < 7) {                            /* own address, random port */
            f->src_ip = (uint32_t)(r >> 32);
            f->src_port = (uint16_t)(1024 + (rnd64() % 64512));
        } else {                                             /* behind NAT: 64 addresses, sequential ports */
            uint32_t a = nat % 64, j = nat / 64;             /* each address hands out ports 1024, 1025, ... */
            f->src_ip = 0xCB007100u + a + 64u * (j / 64512);  /* 203.0.113.0/26 (next block if ports run out) */
            f->src_port = (uint16_t)(1024 + j % 64512);
            nat++;
        }
        k[i].hash = tabhash_flow(th, f);
    }
}

/* ---- timing ---- */

static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double median(double *v, int n)
{
    qsort(v, (size_t)n, sizeof *v, cmp_double);
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

/* ---- the flow table under test ---- */

static void ft_free(gen_t *g, flow_table_t *ft)
{
    gen_mfree(g, ft->slots);
    gen_mfree(g, ft->entries);
    gen_mfree(g, ft);
}

/* The tuner's flows never expire: every timeout is as long as it can be, and the time stays 0 (one service, 0) */
static const svc_timeouts_t NEVER_EXPIRE[1] = { { UINT64_MAX, UINT64_MAX, UINT64_MAX } };

static flow_table_t *ft_new(gen_t *g, uint32_t nb, uint32_t bs, uint32_t depth, uint32_t budget,
                            const tabhash_t *th)
{
    flow_table_t *ft = ft_init(g, nb, bs, depth, budget, 0, HASH_TAB, th, 0, NEVER_EXPIRE);  /* sig_bits 0: automatic, as in the LB */
    if (!ft) {
        fprintf(stderr, "autotune: ft_init(%u buckets x %u) failed: over the memory budget?\n", nb, bs);
        exit(1);
    }
    return ft;
}

/* Inserts keys until the first failure; returns how many were stored. The time stays 0: nothing ever expires. */
static uint32_t fill_until_failure(flow_table_t *ft, const tkey_t *k, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (!ft_insert(ft, &k[i].key, 0, 1, k[i].hash, 0, 0))
            return i;
    return n;
}

typedef struct {
    uint32_t bs, nb, depth, budget;
    double min_lf;                 /* usable load factor of the chosen search, worst seed */
    int feasible;
    double t_hit, t_miss, t_ins, t_over, f_over, cost;
} cand_t;

int main(void)
{
    gen_t *g = gen_open(NULL, "mode=bench,results=/dev/null,pool_mb=8,bench_keys=1k", &bench_ops);
    if (!g) {
        fprintf(stderr, "autotune: gen_open failed\n");
        return 1;
    }
    size_t budget_bytes = gen_lb(g)->mem_budget;
    if (budget_bytes <= RESERVE_BYTES + (1u << 20)) {
        fprintf(stderr, "autotune: memory budget %zu B too small\n", budget_bytes);
        return 1;
    }
    uint64_t slots = (budget_bytes - RESERVE_BYTES) / SLOT_BYTES;
    double target_lf = (double)TARGET_FLOWS / (double)slots;
    fprintf(stderr, "autotune: budget %zu B, reserve %u B -> %llu slots (%zu B each); target %u flows = load %.3f\n",
            budget_bytes, RESERVE_BYTES, (unsigned long long)slots, (size_t)SLOT_BYTES, TARGET_FLOWS, target_lf);
    if (target_lf * (1 + MARGIN) >= 1.0) {
        fprintf(stderr, "autotune: the target does not fit the budget\n");
        return 1;
    }

    /* Tabulation tables and keys: the tuner's own working data, not LB state, so plain malloc. */
    tabhash_t *th = malloc(sizeof *th);
    uint32_t n_keys = (uint32_t)slots + N_LOOKUP;           /* enough to fill a table, plus never-inserted keys */
    tkey_t *keys = malloc((size_t)n_keys * sizeof *keys);
    tkey_t *hitk = malloc((size_t)N_LOOKUP * sizeof *hitk); /* the keys of the timed hits, in lookup order */
    if (!th || !keys || !hitk) {
        fprintf(stderr, "autotune: out of memory\n");
        return 1;
    }

    cand_t cands[LEN(BUCKET_SIZES)];
    volatile uintptr_t sink = 0;

    /* 1. per layout, the smallest search (budget first, then depth) whose usable capacity clears the target */
    for (size_t c = 0; c < LEN(BUCKET_SIZES); c++) {
        cand_t *cd = &cands[c];
        memset(cd, 0, sizeof *cd);
        cd->bs = BUCKET_SIZES[c];
        cd->nb = (uint32_t)(slots / cd->bs);

        for (size_t bi = 0; bi < LEN(BUDGETS) && !cd->feasible; bi++) {
            for (size_t di = 0; di < LEN(DEPTHS) && !cd->feasible; di++) {
                double worst = 1.0;
                for (int s = 0; s < FILL_SEEDS; s++) {
                    tabhash_init(th, 1000 + (uint64_t)s);
                    make_keys(keys, (uint32_t)slots, 2000 + (uint64_t)s, th);
                    flow_table_t *ft = ft_new(g, cd->nb, cd->bs, DEPTHS[di], BUDGETS[bi], th);
                    double lf = (double)fill_until_failure(ft, keys, (uint32_t)slots) / ((double)cd->nb * cd->bs);
                    if (lf < worst)
                        worst = lf;
                    ft_free(g, ft);
                }
                fprintf(stderr, "  B=%2u depth=%u budget=%3u: usable load %.3f\n",
                        cd->bs, DEPTHS[di], BUDGETS[bi], worst);
                if (worst * cd->nb * cd->bs >= TARGET_FLOWS * (1 + MARGIN)) {
                    cd->feasible = 1;
                    cd->depth = DEPTHS[di];
                    cd->budget = BUDGETS[bi];
                    cd->min_lf = worst;
                }
            }
        }
        if (!cd->feasible)
            fprintf(stderr, "  B=%2u: no search reaches the target + margin: rejected\n", cd->bs);
    }

    /* 2. speed at the target load, each layout with its search. Run r measures every feasible layout in turn, on
          the same keys, starting from a different layout each run: the layouts alternate rather than being timed one
          after the other (brief, "Measuring properly"). The keys of the hits are copied, in random order, into hitk
          before the timing, so that the timed loop reads them in sequence: in the LB the key comes from the packet,
          already in cache, whereas a random read in the 45 MB key array would add the same cache miss to every
          layout and hide their differences. */
    double th_v[LEN(BUCKET_SIZES)][REPS], tm_v[LEN(BUCKET_SIZES)][REPS], ti_v[LEN(BUCKET_SIZES)][REPS];
    double tf_v[LEN(BUCKET_SIZES)][REPS], ff_v[LEN(BUCKET_SIZES)][REPS];
    for (int r = 0; r < REPS; r++) {
        tabhash_init(th, 3000 + (uint64_t)r);
        make_keys(keys, n_keys, 4000 + (uint64_t)r, th);
        rng_state = 5000 + (uint64_t)r;
        for (uint32_t i = 0; i < N_LOOKUP; i++)       /* hits: stored keys in random order (independent lookups) */
            hitk[i] = keys[rnd64() % TARGET_FLOWS];

        for (size_t j = 0; j < LEN(BUCKET_SIZES); j++) {
            size_t c = (j + (size_t)r) % LEN(BUCKET_SIZES);
            cand_t *cd = &cands[c];
            if (!cd->feasible)
                continue;
            flow_table_t *ft = ft_new(g, cd->nb, cd->bs, cd->depth, cd->budget, th);

            uint32_t pre = TARGET_FLOWS - N_INSERT;
            for (uint32_t i = 0; i < pre; i++)
                ft_insert(ft, &keys[i].key, 0, 1, keys[i].hash, 0, 0);
            double t0 = now_ns();
            for (uint32_t i = pre; i < TARGET_FLOWS; i++)
                sink += (uintptr_t)ft_insert(ft, &keys[i].key, 0, 1, keys[i].hash, 0, 0);
            ti_v[c][r] = (now_ns() - t0) / N_INSERT;

            t0 = now_ns();
            for (uint32_t i = 0; i < N_LOOKUP; i++)
                sink += (uintptr_t)ft_find(ft, &hitk[i].key, hitk[i].hash, 0);
            th_v[c][r] = (now_ns() - t0) / N_LOOKUP;

            /* misses: keys never inserted */
            t0 = now_ns();
            for (uint32_t i = 0; i < N_LOOKUP; i++) {
                const tkey_t *k = &keys[(uint32_t)slots + i];
                sink += (uintptr_t)ft_find(ft, &k->key, k->hash, 0);
            }
            tm_v[c][r] = (now_ns() - t0) / N_LOOKUP;

            /* a full table (what a SYN flood leads to): fill until the first failure, then time N_OVER more insert
               attempts. Most still succeed at first, then fewer and fewer: report the cost per attempt and the
               share that failed. The search bound caps the cost of each attempt. */
            uint32_t at = TARGET_FLOWS + fill_until_failure(ft, keys + TARGET_FLOWS, (uint32_t)slots - TARGET_FLOWS);
            uint32_t n_try = 0, n_fail = 0;
            t0 = now_ns();
            for (uint32_t i = at; i < at + N_OVER && i < n_keys; i++, n_try++)
                n_fail += !ft_insert(ft, &keys[i].key, 0, 1, keys[i].hash, 0, 0);
            tf_v[c][r] = n_try ? (now_ns() - t0) / n_try : 0;
            ff_v[c][r] = n_try ? (double)n_fail / n_try : 0;

            ft_free(g, ft);
        }
    }
    for (size_t c = 0; c < LEN(BUCKET_SIZES); c++) {
        cand_t *cd = &cands[c];
        if (!cd->feasible)
            continue;
        cd->t_hit = median(th_v[c], REPS);
        cd->t_miss = median(tm_v[c], REPS);
        cd->t_ins = median(ti_v[c], REPS);
        cd->t_over = median(tf_v[c], REPS);
        cd->f_over = median(ff_v[c], REPS);
        cd->cost = HIT_FRACTION * cd->t_hit + (1 - HIT_FRACTION) * (cd->t_miss + cd->t_ins);
    }

    /* the choice: the lowest cost, ties (within TIE of it) broken by the cheapest insert into a full table */
    double lowest = 0;
    for (size_t c = 0; c < LEN(BUCKET_SIZES); c++)
        if (cands[c].feasible && (lowest == 0 || cands[c].cost < lowest))
            lowest = cands[c].cost;
    const cand_t *best = NULL;
    fprintf(stderr, "\n  B   buckets  depth budget usable  hit_ns miss_ns ins_ns  cost_ns | full table: ins_ns failed\n");
    for (size_t c = 0; c < LEN(BUCKET_SIZES); c++) {
        const cand_t *cd = &cands[c];
        if (!cd->feasible) {
            fprintf(stderr, "  %-3u %8u  (rejected: usable capacity below target + margin)\n", cd->bs, cd->nb);
            continue;
        }
        fprintf(stderr, "  %-3u %8u  %5u %6u %6.3f %7.1f %7.1f %6.1f %8.1f |            %6.1f %5.1f%%\n", cd->bs,
                cd->nb, cd->depth, cd->budget, cd->min_lf, cd->t_hit, cd->t_miss, cd->t_ins, cd->cost, cd->t_over,
                100 * cd->f_over);
        if (cd->cost <= lowest * (1 + TIE) && (!best || cd->t_over < best->t_over))
            best = cd;
    }
    if (!best) {
        fprintf(stderr, "autotune: no layout reaches the target load\n");
        return 1;
    }
    fprintf(stderr, "autotune: best B=%u (among the layouts that hold %u flows + %.0f %%, cost within %.0f %% of the "
            "lowest, the cheapest insert into a full table)\n", best->bs, TARGET_FLOWS, MARGIN * 100, TIE * 100);

    free(hitk);
    free(keys);
    free(th);
    gen_close(g);
    (void)sink;

    /* the one line on standard output */
    printf("ft_buckets=%u,bucket_size=%u,max_depth=%u,dfs_budget=%u\n", best->nb, best->bs, best->depth, best->budget);
    return 0;
}

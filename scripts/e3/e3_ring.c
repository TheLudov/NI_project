/* scripts/e3/e3_ring.c: E3, replica selection, measured on the ring alone (no harness traffic).
 *
 * Author: Atieh El Gebrin
 *
 * For each service of the workload (chat 64 replicas, video 32, web 48) and each number of points per replica
 * v in {1, 10, 100, 1000}, with SEEDS different secret seeds (the seed decides both the flow hash, simple
 * tabulation as in the LB, and the positions of the ring's points), it measures:
 *
 *   ring_maxavg   the largest share of the ring any replica owns, times N: the balance by flows when there are
 *                 many flows (the limit as the number of flows grows). Exact: computed from the arcs.
 *   flows_maxavg  most loaded / average replica with the service's real number of concurrent flows (chat 500k,
 *                 video 2.5k, web 5k), realistic 5-tuples (70 % own address and random port, 30 % behind 64 NAT
 *                 addresses with sequential ports): ring imbalance plus the luck of the draw.
 *   add_*         share of the flows that move when one replica is added (the new replica's share of the new
 *                 ring: exact), over ADDS fresh addresses: mean, min, max; ideal 1 / (N + 1).
 *   rm_*          share of the flows that move when one replica is removed (its share before: exact), over every
 *                 replica: mean, min, max; ideal 1 / N.
 *   moved_wrong   flows (of the sampled ones) that moved anywhere else than to the added replica, or that moved
 *                 without having been on the removed one: 0 for a consistent hash.
 *   mem_bytes     memory ring_init allocates for the service (points, replica table, id map, scratch).
 *   lookup_ns     time of one ring_lookup, on independent random hashes (as in a batch), median of REPS.
 *   add_event_us  time ring_event takes to add one replica (generate, sort and merge its v points), and
 *   rm_event_us   to remove one (one pass over the points): medians over the ADDS added replicas. This is what a
 *                 replica event costs the LB's loop, which handles events between batches.
 *
 * One CSV line per (service, v, seed) on standard output; scripts/e3/e3_plot.py reads it.
 *
 *   cc -std=c11 -O2 -msse4.2 -Isrc -Igen scripts/e3/e3_ring.c src/ring.c src/tabhash.c -o build/e3_ring
 *   taskset -c 0 build/e3_ring [SEEDS] > results.csv        (scripts/e3/run_e3.sh does both)
 *
 * The ring's allocations go to a stand-in gen_alloc (malloc, with a byte count), so it needs no harness.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gen.h"
#include "ring.h"
#include "tabhash.h"

/* ---- stand-in allocator: the ring's memory, counted ---- */

static size_t live_bytes;
typedef struct { size_t bytes; void *raw; } hdr_t;

void *gen_alloc(gen_t *g, size_t bytes, size_t align, int huge)
{
    (void)g; (void)huge;
    if (align < 64)
        align = 64;
    void *raw = malloc(bytes + align + sizeof(hdr_t));
    if (!raw)
        return NULL;
    uintptr_t p = ((uintptr_t)raw + sizeof(hdr_t) + align - 1) & ~(uintptr_t)(align - 1);
    hdr_t *h = (hdr_t *)p - 1;
    h->bytes = bytes;
    h->raw = raw;
    live_bytes += bytes;
    return (void *)p;
}

void gen_mfree(gen_t *g, void *ptr)
{
    (void)g;
    if (!ptr)
        return;
    hdr_t *h = (hdr_t *)ptr - 1;
    live_bytes -= h->bytes;
    free(h->raw);
}

static void ring_free(ring_t *r)
{
    gen_mfree(NULL, r->pts);
    gen_mfree(NULL, r->reps);
    gen_mfree(NULL, r->idmap);
    gen_mfree(NULL, r->scratch);
}

/* ---- the workload ---- */

typedef struct {
    const char *name;
    uint32_t vip, n_replicas, first_ip, flows;
} svc_def_t;

static const svc_def_t SERVICES[] = {
    { "chat",  0xC633640Au, 64, 0x0A010001u, 500000 },   /* 198.51.100.10, 10.1.0.1 .. */
    { "video", 0xC6336414u, 32, 0x0A020001u,   2500 },   /* 198.51.100.20, 10.2.0.1 .. */
    { "web",   0xC633641Eu, 48, 0x0A030001u,   5000 },   /* 198.51.100.30, 10.3.0.1 .. */
};
static const uint32_t VS[] = { 1, 10, 100, 1000 };
#define LEN(a) (sizeof(a) / sizeof((a)[0]))
#define ADDS 16           /* fresh replicas tried for the "add" measurement */
#define REPS 5            /* timing repetitions */
#define N_TIMED (1u << 22) /* lookups per timing */

static uint64_t rng_state;
static uint64_t rnd64(void)        /* splitmix64 */
{
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Realistic flow hashes for one service: the low 32 bits of the LB's tabulation hash, which is what
   compute_replica gives ring_lookup. Same client mix as the autotuner (and E2). */
static void make_hashes(uint32_t *h, uint32_t n, uint32_t vip, const tabhash_t *th)
{
    uint32_t nat = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t r = rnd64();
        flow_key_t k;
        memset(&k, 0, sizeof k);
        k.dst_ip = vip;
        k.dst_port = 443;
        k.protocol = 6;
        if ((r >> 10) % 10 < 7) {
            k.src_ip = (uint32_t)(r >> 32);
            k.src_port = (uint16_t)(1024 + rnd64() % 64512);
        } else {
            uint32_t a = nat % 64, j = nat / 64;
            k.src_ip = 0xCB007100u + a + 64u * (j / 64512);
            k.src_port = (uint16_t)(1024 + j % 64512);
            nat++;
        }
        h[i] = (uint32_t)tabhash_flow(th, &k);
    }
}

/* The share of the ring each replica owns: point i owns (h[i-1], h[i]], point 0 also the wrap-around arc. */
static void shares(const ring_t *r, const uint32_t *ips, uint32_t n_ips, double *share)
{
    for (uint32_t k = 0; k < n_ips; k++)
        share[k] = 0;
    for (uint32_t i = 0; i < r->n_pts; i++) {
        uint64_t prev = i ? r->pts[i - 1].h : (uint64_t)r->pts[r->n_pts - 1].h;
        uint64_t arc = i ? (uint64_t)r->pts[i].h - prev : (uint64_t)r->pts[0].h + (1ull << 32) - prev;
        for (uint32_t k = 0; k < n_ips; k++)
            if (ips[k] == r->pts[i].ip) {
                share[k] += (double)arc / 4294967296.0;
                break;
            }
    }
}

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

int main(int argc, char **argv)
{
    int seeds = argc > 1 ? atoi(argv[1]) : 7;
    static tabhash_t th;
    uint32_t max_flows = 0;
    for (size_t s = 0; s < LEN(SERVICES); s++)
        if (SERVICES[s].flows > max_flows)
            max_flows = SERVICES[s].flows;
    uint32_t *fh = malloc((size_t)max_flows * sizeof *fh);      /* the flows' hashes */
    uint32_t *before = malloc((size_t)max_flows * sizeof *before);
    uint32_t *timed = malloc((size_t)N_TIMED * sizeof *timed);   /* random hashes for the timing */
    if (!fh || !before || !timed)
        return 1;
    volatile uint32_t sink = 0;

    printf("service,N,v,seed,ring_maxavg,flows,flows_maxavg,add_mean,add_min,add_max,add_ideal,"
           "rm_mean,rm_min,rm_max,rm_ideal,moved_wrong,mem_bytes,points_bytes,lookup_ns,add_event_us,rm_event_us\n");
    for (int seed = 1; seed <= seeds; seed++) {
        tabhash_init(&th, (uint64_t)seed * 0x9E3779B97F4A7C15ull);
        for (size_t s = 0; s < LEN(SERVICES); s++) {
            const svc_def_t *d = &SERVICES[s];
            uint32_t N = d->n_replicas;
            replica_t reps[256];
            uint32_t ips[256];
            for (uint32_t k = 0; k < N; k++) {
                reps[k].ipv4 = ips[k] = d->first_ip + k;
                reps[k].state = REPLICA_STATE_ACTIVE;
            }
            service_t svc = { .vip = d->vip, .port = 443, .proto = 6, .n_replicas = N, .replicas = reps };
            rng_state = 1000 * (uint64_t)seed + s;
            make_hashes(fh, d->flows, d->vip, &th);

            for (size_t vi = 0; vi < LEN(VS); vi++) {
                uint32_t v = VS[vi];
                ring_t r;
                size_t mem0 = live_bytes;
                if (!ring_init(&r, NULL, &svc, v, (uint64_t)seed * 0x9E3779B97F4A7C15ull)) {
                    fprintf(stderr, "ring_init failed\n");
                    return 1;
                }
                size_t mem = live_bytes - mem0;

                /* balance: ring shares (exact), then the service's real flows */
                double share[256], mx = 0;
                shares(&r, ips, N, share);
                for (uint32_t k = 0; k < N; k++)
                    if (share[k] > mx)
                        mx = share[k];
                double ring_maxavg = mx * N;

                uint32_t cnt[256] = { 0 };
                for (uint32_t f = 0; f < d->flows; f++) {
                    before[f] = ring_lookup(&r, fh[f]);
                    cnt[before[f] - d->first_ip]++;
                }
                uint32_t cmax = 0;
                for (uint32_t k = 0; k < N; k++)
                    if (cnt[k] > cmax)
                        cmax = cnt[k];
                double flows_maxavg = (double)cmax / ((double)d->flows / N);

                /* moved on add: the new replica's share of the new ring, over ADDS fresh addresses */
                double add_sum = 0, add_min = 1, add_max = 0, t_add[ADDS], t_rm[ADDS];
                uint64_t wrong = 0;
                for (int a = 0; a < ADDS; a++) {
                    uint32_t ip = d->first_ip + 200 + (uint32_t)a;   /* after the service's range */
                    double te = now_ns();
                    if (!ring_event(&r, REPLICA_ADD, ip))
                        return 1;
                    t_add[a] = (now_ns() - te) / 1000;
                    double sh;
                    shares(&r, &ip, 1, &sh);
                    add_sum += sh;
                    if (sh < add_min) add_min = sh;
                    if (sh > add_max) add_max = sh;
                    for (uint32_t f = 0; f < d->flows; f++) {      /* consistency, on the sampled flows */
                        uint32_t now = ring_lookup(&r, fh[f]);
                        if (now != before[f] && now != ip)
                            wrong++;
                    }
                    te = now_ns();
                    if (!ring_event(&r, REPLICA_DOWN, ip))       /* back to the N replicas */
                        return 1;
                    t_rm[a] = (now_ns() - te) / 1000;
                }
                qsort(t_add, ADDS, sizeof *t_add, cmp_double);
                qsort(t_rm, ADDS, sizeof *t_rm, cmp_double);

                /* moved on remove: each replica's share before (exact), over every replica */
                double rm_sum = 0, rm_min = 1, rm_max = 0;
                for (uint32_t k = 0; k < N; k++) {
                    rm_sum += share[k];
                    if (share[k] < rm_min) rm_min = share[k];
                    if (share[k] > rm_max) rm_max = share[k];
                }
                for (uint32_t k = 0; k < N; k += 8) {              /* consistency, for a few removals */
                    if (!ring_event(&r, REPLICA_DOWN, ips[k]))
                        return 1;
                    for (uint32_t f = 0; f < d->flows; f++) {
                        uint32_t now = ring_lookup(&r, fh[f]);
                        if (now != before[f] && before[f] != ips[k])
                            wrong++;
                    }
                    if (!ring_event(&r, REPLICA_UP, ips[k]))
                        return 1;
                }

                /* lookup time on independent random hashes (median of REPS) */
                double t[REPS];
                for (int rep = 0; rep < REPS; rep++) {
                    for (uint32_t i = 0; i < N_TIMED; i++)
                        timed[i] = (uint32_t)rnd64();
                    double t0 = now_ns();
                    for (uint32_t i = 0; i < N_TIMED; i++)
                        sink += ring_lookup(&r, timed[i]);
                    t[rep] = (now_ns() - t0) / N_TIMED;
                }
                qsort(t, REPS, sizeof *t, cmp_double);

                printf("%s,%u,%u,%d,%.4f,%u,%.4f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%llu,%zu,%zu,%.2f,%.3f,%.3f\n",
                       d->name, N, v, seed, ring_maxavg, d->flows, flows_maxavg, add_sum / ADDS, add_min, add_max,
                       1.0 / (N + 1), rm_sum / N, rm_min, rm_max, 1.0 / N, (unsigned long long)wrong, mem,
                       (size_t)N * v * sizeof(ring_point_t), t[REPS / 2], t_add[ADDS / 2], t_rm[ADDS / 2]);
                fflush(stdout);
                ring_free(&r);
            }
        }
    }
    (void)sink;
    free(fh);
    free(before);
    free(timed);
    return 0;
}

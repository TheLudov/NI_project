/* Responsible: Atieh El Gebrin.
   Unit tests for src/ring.c / src/ring.h, linked WITHOUT libgen.a:
   gen_alloc/gen_mfree are replaced by a malloc-based fake that can be told to fail. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ring.h"
#include "crc32c.h"

/* ---- fake allocator ---- */
static long live_blocks, live_bytes, peak_bytes, fail_after = -1, n_allocs;
typedef struct { size_t sz; } hdr_t;
void *gen_alloc(gen_t *g, size_t bytes, size_t align, int huge)
{
    (void)g; (void)huge;
    n_allocs++;
    if (fail_after == 0) return NULL;
    if (fail_after > 0) fail_after--;
    if (align < 64) align = 64;
    void *p = NULL;
    if (posix_memalign(&p, align, bytes + align)) return NULL;
    ((hdr_t *)p)->sz = bytes;
    memset((char *)p + align, 0xA5, bytes);          /* garbage, like fresh memory */
    live_blocks++; live_bytes += (long)bytes; if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    return (char *)p + align;
}
void gen_mfree(gen_t *g, void *ptr)
{
    (void)g;
    if (!ptr) return;
    hdr_t *h = (hdr_t *)((char *)ptr - 64);
    live_blocks--; live_bytes -= (long)h->sz;
    free(h);
}

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; if (failures < 20) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr);} } } while (0)

static uint64_t rng_s = 88172645463325252ull;
static uint64_t rnd(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17; return rng_s; }

/* reference: first point with h >= hash, wrapping; linear scan */
static uint32_t ref_lookup(const ring_t *r, uint32_t hash)
{
    if (r->n_pts == 0) return 0;
    for (uint32_t i = 0; i < r->n_pts; i++)
        if (r->pts[i].h >= hash) return r->pts[i].ip;
    return r->pts[0].ip;
}

static int sorted_ok(const ring_t *r)
{
    for (uint32_t i = 1; i < r->n_pts; i++) {
        const ring_point_t *a = &r->pts[i - 1], *b = &r->pts[i];
        if (!(a->h < b->h || (a->h == b->h && a->ip <= b->ip))) return 0;
    }
    return 1;
}

static int cmp_pt(const void *pa, const void *pb)
{
    const ring_point_t *a = pa, *b = pb;
    if (a->h != b->h) return a->h < b->h ? -1 : 1;
    if (a->ip != b->ip) return a->ip < b->ip ? -1 : 1;
    return 0;
}

/* forward */
static void free_ring(ring_t *r) { gen_mfree(NULL, r->pts); gen_mfree(NULL, r->reps); gen_mfree(NULL, r->scratch);
#ifdef HAVE_IDS
    gen_mfree(NULL, r->idmap);
#endif
}

/* full rebuild of r's ring from its active replicas, with a fresh ring_init on a synthetic service */
static int same_as_rebuild(const ring_t *r)
{
    replica_t *reps = malloc(sizeof *reps * (r->n_reps + 1));
    for (uint32_t i = 0; i < r->n_reps; i++) reps[i] = r->reps[i];
    service_t svc = { .vip = 1, .port = 1, .proto = 6, .n_replicas = r->n_reps, .replicas = reps };
    ring_t fresh;
    if (!ring_init(&fresh, NULL, &svc, r->v, r->seed)) { free(reps); return 0; }
    int ok = fresh.n_pts == r->n_pts && memcmp(fresh.pts, r->pts, (size_t)r->n_pts * sizeof *r->pts) == 0;
    free_ring(&fresh);
    free(reps);
    return ok;
}

static uint32_t count_active(const ring_t *r)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < r->n_reps; i++) n += r->reps[i].state == REPLICA_STATE_ACTIVE;
    return n;
}


/* ---- 1. lookup against the reference, on crafted rings with ties and edge hashes ---- */
static void test_lookup_crafted(void)
{
    ring_t r; memset(&r, 0, sizeof r);
    for (uint32_t n = 0; n <= 300; n++) {
        r.pts = malloc(sizeof *r.pts * (n + 1));
        r.n_pts = n;
        /* small value range forces many ties in h */
        uint32_t range = (n % 3 == 0) ? 8 : (n % 3 == 1 ? 1000 : 0xFFFFFFFFu);
        for (uint32_t i = 0; i < n; i++) {
            r.pts[i].h = range == 0xFFFFFFFFu ? (uint32_t)rnd() : (uint32_t)(rnd() % range);
            if (n > 5 && i == 0) r.pts[i].h = 0;
            if (n > 5 && i == 1) r.pts[i].h = 0xFFFFFFFFu;
            r.pts[i].ip = 1 + (uint32_t)(rnd() % 50);
        }
        qsort(r.pts, n, sizeof *r.pts, cmp_pt);
        uint32_t probes[] = { 0, 1, 2, 7, 8, 9, 500, 999, 1000, 0x7FFFFFFFu, 0xFFFFFFFEu, 0xFFFFFFFFu };
        for (size_t k = 0; k < sizeof probes / sizeof *probes; k++)
            CHECK(ring_lookup(&r, probes[k]) == ref_lookup(&r, probes[k]), "crafted n=%u probe=%u", n, probes[k]);
        for (uint32_t i = 0; i < n; i++) {               /* exactly on, just before, just after each point */
            uint32_t h = r.pts[i].h;
            CHECK(ring_lookup(&r, h) == ref_lookup(&r, h), "on point n=%u i=%u", n, i);
            CHECK(ring_lookup(&r, h - 1) == ref_lookup(&r, h - 1), "before point n=%u", n);
            CHECK(ring_lookup(&r, h + 1) == ref_lookup(&r, h + 1), "after point n=%u", n);
        }
        for (int k = 0; k < 2000; k++) {
            uint32_t h = range == 0xFFFFFFFFu ? (uint32_t)rnd() : (uint32_t)(rnd() % (range + 2));
            CHECK(ring_lookup(&r, h) == ref_lookup(&r, h), "random n=%u h=%u", n, h);
        }
        free(r.pts);
    }
}

/* ---- 2. random event sequences: incremental ring == full rebuild, after every event ---- */
static void test_events_random(uint32_t v, int n_init, int n_events)
{
    replica_t init[600];
    for (int i = 0; i < n_init; i++) {
        init[i].ipv4 = 0x0A010001u + (uint32_t)i;
        uint64_t x = rnd() % 10;
        init[i].state = x < 7 ? REPLICA_STATE_ACTIVE : (x < 9 ? REPLICA_STATE_DRAINING : REPLICA_STATE_DOWN);
    }
    service_t svc = { 1, 1, 6, (uint32_t)n_init, init };
    ring_t a, b;
    uint64_t seed = rnd();
    CHECK(ring_init(&a, NULL, &svc, v, seed), "init a");
    CHECK(ring_init(&b, NULL, &svc, v, seed), "init b");
    CHECK(same_as_rebuild(&a), "initial");
    uint32_t next_ip = 0x0A010001u + (uint32_t)n_init;
    for (int e = 0; e < n_events; e++) {
        int type = (int)(rnd() % 4);
        uint32_t ip;
        uint64_t pick = rnd() % 10;
        if (pick == 0) ip = next_ip++;                           /* a fresh address */
        else if (pick == 1) ip = 0x0B000000u + (uint32_t)(rnd() % 5); /* unknown for drain/down */
        else ip = a.n_reps ? a.reps[rnd() % a.n_reps].ipv4 : next_ip++;
        CHECK(ring_event(&a, type, ip) == 1, "event a");
        CHECK(ring_event(&b, type, ip) == 1, "event b");
        CHECK(sorted_ok(&a), "sorted after event %d", e);
        CHECK(a.n_pts == count_active(&a) * a.v, "n_pts=%u active=%u v=%u", a.n_pts, count_active(&a), a.v);
        CHECK(ring_n_active(&a) == count_active(&a), "ring_n_active");
        CHECK(a.n_pts == b.n_pts && memcmp(a.pts, b.pts, (size_t)a.n_pts * sizeof *a.pts) == 0, "a != b");
        if (e % 7 == 0) CHECK(same_as_rebuild(&a), "rebuild differs after event %d (type %d)", e, type);
#ifdef HAVE_IDS
        for (uint32_t i = 0; i < a.n_reps; i++) {
            CHECK(ring_replica_id(&a, a.reps[i].ipv4) == i, "id of rep %u", i);
            CHECK(ring_ip(&a, i) == a.reps[i].ipv4 && ring_state(&a, i) == a.reps[i].state, "ip/state of id");
        }
        CHECK(ring_replica_id(&a, 0xDEAD0000u + (uint32_t)(rnd() % 1000)) == RING_NO_ID, "unknown id");
        if (type == REPLICA_ADD || type == REPLICA_UP)
            CHECK(ring_replica_id(&a, ip) != RING_NO_ID && ring_state(&a, ring_replica_id(&a, ip)) == REPLICA_STATE_ACTIVE, "added ip known and active");
#endif
        /* states and lookups: never a non-active replica */
        for (int k = 0; k < 50; k++) {
            uint32_t h = (uint32_t)rnd();
            uint32_t got = ring_lookup(&a, h);
            CHECK(got == ref_lookup(&a, h), "lookup vs ref");
            if (got) {
                int st = -1;
                for (uint32_t i = 0; i < a.n_reps; i++) if (a.reps[i].ipv4 == got) st = a.reps[i].state;
                CHECK(st == REPLICA_STATE_ACTIVE, "lookup gave a non-active replica (state %d)", st);
            } else
                CHECK(count_active(&a) == 0, "0 with active replicas");
        }
    }
    free_ring(&a); free_ring(&b);
}

/* ---- 3. alloc failure inside ring_event ---- */
static void test_alloc_failure(void)
{
    replica_t init[4] = { {0x0A000001u, 0}, {0x0A000002u, 0}, {0x0A000003u, 0}, {0x0A000004u, 0} };
    service_t svc = { 1, 1, 6, 4, init };
    for (int scenario = 0; scenario < 3; scenario++) {
        ring_t r;
        CHECK(ring_init(&r, NULL, &svc, 100, 7), "init");
        if (scenario == 2) ring_event(&r, REPLICA_DOWN, 0x0A000002u);
        /* force both arrays to be full, so the next ADD must grow them */
        r.cap_reps = r.n_reps; r.cap_pts = r.n_pts;
        uint32_t n_pts = r.n_pts, n_reps = r.n_reps;
        ring_point_t *copy = malloc(sizeof *copy * n_pts); memcpy(copy, r.pts, sizeof *copy * n_pts);
        fail_after = scenario == 0 ? 0 : 1;   /* 0: the reps grow fails; 1: reps ok, pts grow fails */
        uint32_t ip = scenario == 2 ? 0x0A000002u : 0x0A0F0F0Fu;
        if (scenario == 2) fail_after = 0;    /* known replica: only the pts grow, and it fails */
        int ok = ring_event(&r, scenario == 2 ? REPLICA_UP : REPLICA_ADD, ip);
        fail_after = -1;
        int unchanged = r.n_pts == n_pts && !memcmp(copy, r.pts, sizeof *copy * n_pts);
        const char *what[] = { "new replica, reps grow fails", "new replica, pts grow fails", "known DOWN replica UP, pts grow fails" };
        printf("  %-40s returned %d, points unchanged: %s, n_reps %u -> %u", what[scenario], ok, unchanged ? "yes" : "NO", n_reps, r.n_reps);
        for (uint32_t i = 0; i < r.n_reps; i++) if (r.reps[i].ipv4 == ip) printf(", its state now %d", r.reps[i].state);
        putchar('\n');
        /* a retry once memory is back must work */
        CHECK(ring_event(&r, scenario == 2 ? REPLICA_UP : REPLICA_ADD, ip) == 1, "retry");
        CHECK(same_as_rebuild(&r), "after retry");
        CHECK(r.n_pts == count_active(&r) * r.v, "count after retry");
        free(copy);
        free_ring(&r);
    }
}

/* ---- 4. balance and movement with the LB's real flow hash (crc32c as in lb/main.c) ---- */
static uint32_t flow_hash(uint64_t seed, uint32_t sip, uint32_t dip, uint16_t sp, uint16_t dp, uint8_t proto)
{
    uint64_t ips = ((uint64_t)sip << 32) | dip;
    uint32_t ports = ((uint32_t)sp << 16) | dp;
    uint32_t h = crc32c_u64(~0u, seed);
    h = crc32c_u64(h, ips);
    h = crc32c_u32(h, ports);
    return crc32c_u8(h, proto);
}

static void balance(uint32_t n_rep, uint32_t v, uint32_t n_flows, int nat, uint64_t seed)
{
    replica_t *init = malloc(sizeof *init * (n_rep + 1));
    for (uint32_t i = 0; i < n_rep; i++) { init[i].ipv4 = 0x0A010001u + i; init[i].state = 0; }
    service_t svc = { 0xC6336410u, 443, 6, n_rep, init };
    ring_t r;
    if (!ring_init(&r, NULL, &svc, v, seed)) { puts("init failed"); return; }
    uint32_t *cnt = calloc(n_rep + 2, sizeof *cnt);
    uint32_t *before = malloc(sizeof *before * n_flows);
    uint32_t *hs = malloc(sizeof *hs * n_flows);
    for (uint32_t f = 0; f < n_flows; f++) {
        uint32_t sip; uint16_t sp;
        if (nat && f % 10 < 3) { sip = 0x50000000u + (f % 1000); sp = (uint16_t)(1024 + f / 1000); }  /* NAT: sequential ports */
        else { sip = (uint32_t)rnd(); sp = (uint16_t)(32768 + rnd() % 28232); }
        hs[f] = flow_hash(seed, sip, 0xC6336410u, sp, 443, 6);
        before[f] = ring_lookup(&r, hs[f]);
        cnt[before[f] - 0x0A010001u]++;
    }
    uint32_t mx = 0; for (uint32_t i = 0; i < n_rep; i++) if (cnt[i] > mx) mx = cnt[i];
    double avg = (double)n_flows / n_rep;
    /* add one replica */
    ring_event(&r, REPLICA_ADD, 0x0A010001u + n_rep);
    uint32_t moved_add = 0, wrong = 0;
    for (uint32_t f = 0; f < n_flows; f++) {
        uint32_t now = ring_lookup(&r, hs[f]);
        if (now != before[f]) { moved_add++; if (now != 0x0A010001u + n_rep) wrong++; }
    }
    ring_event(&r, REPLICA_DOWN, 0x0A010001u + n_rep);     /* back to the original set */
    uint32_t back = 0;
    for (uint32_t f = 0; f < n_flows; f++) back += ring_lookup(&r, hs[f]) == before[f];
    /* remove one replica */
    ring_event(&r, REPLICA_DRAIN, 0x0A010001u);
    uint32_t moved_rm = 0, wrong_rm = 0;
    for (uint32_t f = 0; f < n_flows; f++) {
        uint32_t now = ring_lookup(&r, hs[f]);
        if (now != before[f]) { moved_rm++; if (before[f] != 0x0A010001u) wrong_rm++; }
    }
    printf("  N=%3u v=%5u %s: max/avg=%.3f  add: moved %.4f (ideal %.4f, %u not to the new one)  "
           "remove: moved %.4f (ideal %.4f, %u not from the removed one)  restore exact: %s  pts mem=%zu B\n",
           n_rep, v, nat ? "nat " : "rand", mx / avg, (double)moved_add / n_flows, 1.0 / (n_rep + 1), wrong,
           (double)moved_rm / n_flows, (double)cnt[0] / n_flows, wrong_rm, back == n_flows ? "yes" : "NO",
           (size_t)r.cap_pts * sizeof *r.pts);
    free(init); free(cnt); free(before); free(hs); free_ring(&r);
}

/* ---- 5. timings ---- */
static double now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static void timings(uint32_t n_rep, uint32_t v)
{
    replica_t *init = malloc(sizeof *init * n_rep);
    for (uint32_t i = 0; i < n_rep; i++) { init[i].ipv4 = 0x0A030001u + i; init[i].state = 0; }
    service_t svc = { 1, 1, 6, n_rep, init };
    ring_t r;
    if (!ring_init(&r, NULL, &svc, v, 42)) { puts("init failed"); return; }
    enum { L = 2000000 };
    uint32_t *hs = malloc(sizeof *hs * L);
    for (int i = 0; i < L; i++) hs[i] = (uint32_t)rnd();
    volatile uint32_t sink = 0;
    double t0 = now_ns();
    for (int i = 0; i < L; i++) sink += ring_lookup(&r, hs[i]);
    double t1 = now_ns();
    /* events: drain then up (remove + add) on random replicas */
    int E = n_rep > 200 ? 200 : 400;
    double tr = 0, ta = 0;
    for (int e = 0; e < E; e++) {
        uint32_t ip = 0x0A030001u + (uint32_t)(rnd() % n_rep);
        double a = now_ns(); ring_event(&r, REPLICA_DRAIN, ip); double b = now_ns();
        ring_event(&r, REPLICA_UP, ip); double c = now_ns();
        tr += b - a; ta += c - b;
    }
    printf("  N=%4u v=%5u points=%7u (%6zu KB): lookup %.1f ns   drain/down event %.1f us   up/add event %.1f us\n",
           n_rep, v, r.n_pts, (size_t)r.n_pts * 8 / 1024, (t1 - t0) / L, tr / E / 1e3, ta / E / 1e3);
    (void)sink;
    free(hs); free(init); free_ring(&r);
}

int main(int argc, char **argv)
{
    int quick = argc > 1 && !strcmp(argv[1], "quick");
    puts("1. lookup vs brute-force reference (crafted rings, ties, edge hashes)");
    test_lookup_crafted();
    printf("   %s\n", failures ? "FAILURES" : "ok");
    puts("2. random event sequences: incremental == rebuild, main == copy, never a non-active replica");
    int f0 = failures;
    test_events_random(1, 5, 3000);
    test_events_random(3, 1, 3000);
    test_events_random(100, 64, 2000);
    test_events_random(17, 0, 2000);
    if (!quick) test_events_random(1000, 80, 400);
    printf("   %s\n", failures > f0 ? "FAILURES" : "ok");
    puts("3. alloc failure");
    test_alloc_failure();
    if (!quick) {
        puts("4. balance by flows (500k flows) and movement on add/remove");
        uint32_t vs[] = { 1, 10, 100, 1000 };
        for (int nat = 0; nat < 2; nat++)
            for (int i = 0; i < 4; i++) balance(64, vs[i], 500000, nat, 0x1234567890ABCDEFull);
        puts("5. timings (this VM)");
        timings(80, 100); timings(80, 1000); timings(625, 100); timings(625, 1000);
    }
    printf("live blocks at end: %ld\n", live_blocks);
    printf("%s (%d failures)\n", failures ? "FAILED" : "ALL OK", failures);
    return failures != 0;
}

/* ring.c: replica selection, a consistent-hash ring with virtual nodes (one ring per service).
 *
 * Author: Atieh El Gebrin
 */
#include "ring.h"

#include <stdlib.h>
#include <string.h>

/* ---- point positions ---- */

/* splitmix64 finaliser: a strong 64-bit mixer, so that points of nearby addresses (10.1.0.1, 10.1.0.2, ...)
   and consecutive k land far apart on the ring. */
static uint64_t mix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/* Position of point k of replica ip. Depends only on (seed, ip, k), so both LB instances (main and copy)
   and every instance of the tier build the same ring from the same replica set. */
static uint32_t point_hash(uint64_t seed, uint32_t ip, uint32_t k)
{
    return (uint32_t)(mix64(seed ^ mix64(((uint64_t)ip << 32) | k)) >> 32);
}

/* Total order on points: by position, then by address, so that ties are broken the same way everywhere. */
static int point_less(const ring_point_t *a, const ring_point_t *b)
{
    return a->h < b->h || (a->h == b->h && a->ip < b->ip);
}

static int point_cmp(const void *pa, const void *pb)
{
    const ring_point_t *a = pa, *b = pb;
    if (point_less(a, b)) return -1;
    if (point_less(b, a)) return 1;
    return 0;
}

/* ---- growable arrays (all memory through gen_alloc) ---- */

/* Makes room for need elements; copies the used ones. Leaves *arr untouched if gen_alloc fails. */
static int grow(gen_t *g, void **arr, uint32_t *cap, uint32_t used, uint32_t need, size_t elem)
{
    if (need <= *cap)
        return 1;
    uint64_t ncap = *cap ? *cap : 8;
    while (ncap < need)
        ncap *= 2;
    if (ncap > UINT32_MAX)
        return 0;
    void *n = gen_alloc(g, (size_t)ncap * elem, 64, 0);
    if (!n)
        return 0;
    if (*arr) {
        memcpy(n, *arr, (size_t)used * elem);
        gen_mfree(g, *arr);
    }
    *arr = n;
    *cap = (uint32_t)ncap;
    return 1;
}

/* ---- replica ids: address -> index in reps[], open addressing ---- */

static uint32_t idmap_slot(uint32_t ip, uint32_t mask)
{
    return (uint32_t)((ip * 0x9E3779B97F4A7C15ull) >> 32) & mask;
}

uint32_t ring_replica_id(const ring_t *r, uint32_t ipv4)
{
    for (uint32_t j = idmap_slot(ipv4, r->idmap_mask);; j = (j + 1) & r->idmap_mask) {
        uint32_t e = r->idmap[j];
        if (e == 0)
            return RING_NO_ID;
        if (r->reps[e - 1].ipv4 == ipv4)
            return e - 1;
    }
}

static void idmap_put(uint32_t *map, uint32_t mask, uint32_t ip, uint32_t id)
{
    uint32_t j = idmap_slot(ip, mask);
    while (map[j])
        j = (j + 1) & mask;
    map[j] = id + 1;
}

/* Makes the map hold at most half full with n_ids ids, rebuilding it bigger if needed. */
static int idmap_reserve(ring_t *r, uint32_t n_ids)
{
    uint32_t slots = r->idmap ? r->idmap_mask + 1 : 0;
    if ((uint64_t)n_ids * 2 <= slots)
        return 1;
    uint64_t ns = 16;
    while (ns < (uint64_t)n_ids * 2)
        ns *= 2;
    if (ns > (1u << 31))
        return 0;
    uint32_t *map = gen_alloc(r->g, (size_t)ns * sizeof *map, 64, 0);
    if (!map)
        return 0;
    memset(map, 0, (size_t)ns * sizeof *map);
    for (uint32_t i = 0; i < r->n_reps; i++)
        idmap_put(map, (uint32_t)ns - 1, r->reps[i].ipv4, i);
    if (r->idmap)
        gen_mfree(r->g, r->idmap);
    r->idmap = map;
    r->idmap_mask = (uint32_t)ns - 1;
    return 1;
}

/* ---- ring updates ---- */

/* Adds the v points of replica ip: generate, sort, then merge into pts from the back (in place).
   The caller has made room for v more points. */
static void ring_add_points(ring_t *r, uint32_t ip)
{
    for (uint32_t k = 0; k < r->v; k++) {
        r->scratch[k].h = point_hash(r->seed, ip, k);
        r->scratch[k].ip = ip;
    }
    qsort(r->scratch, r->v, sizeof *r->scratch, point_cmp);

    int64_t i = (int64_t)r->n_pts - 1;      /* last old point */
    int64_t j = (int64_t)r->v - 1;          /* last new point */
    int64_t k = (int64_t)r->n_pts + r->v - 1;
    while (j >= 0) {
        if (i >= 0 && point_less(&r->scratch[j], &r->pts[i]))
            r->pts[k--] = r->pts[i--];
        else
            r->pts[k--] = r->scratch[j--];
    }
    r->n_pts += r->v;
}

/* Removes the points of replica ip in one pass, keeping the order. */
static void ring_remove_points(ring_t *r, uint32_t ip)
{
    uint32_t w = 0;
    for (uint32_t i = 0; i < r->n_pts; i++)
        if (r->pts[i].ip != ip)
            r->pts[w++] = r->pts[i];
    r->n_pts = w;
}

/* ---- public ---- */

int ring_init(ring_t *r, gen_t *g, const service_t *svc, uint32_t v, uint64_t seed)
{
    memset(r, 0, sizeof *r);
    r->g = g;
    r->seed = seed;
    r->v = v ? v : 1;

    /* Room, from the start, for the churn of the published scenarios, so that on_event does not allocate:
       a rolling upgrade replaces every initial replica by a fresh address (n more ids), a scale-out adds 25 %,
       plus a few explicit additions. Replicas cost 8 bytes each. */
    uint32_t n = svc->n_replicas;
    if (!grow(g, (void **)&r->reps, &r->cap_reps, 0, 2 * n + n / 4 + 16, sizeof *r->reps))
        return 0;
    if (!idmap_reserve(r, r->cap_reps))
        return 0;
    r->scratch = gen_alloc(g, (size_t)r->v * sizeof *r->scratch, 64, 0);
    if (!r->scratch)
        return 0;

    /* Points: at most n + 25 % (+ 8) replicas are active at once in those scenarios. n counts every initial
       replica, not only the active ones: a draining or down one can come back up. Allocated at the exact size. */
    uint64_t need = (uint64_t)(n + n / 4 + 8) * r->v;
    if (need > UINT32_MAX)
        return 0;
    r->pts = gen_alloc(g, (size_t)need * sizeof *r->pts, 64, 0);
    if (!r->pts)
        return 0;
    r->cap_pts = (uint32_t)need;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t ip = svc->replicas[i].ipv4;
        if (ring_replica_id(r, ip) != RING_NO_ID)
            continue;                              /* listed twice: keep the first */
        r->reps[r->n_reps] = svc->replicas[i];
        idmap_put(r->idmap, r->idmap_mask, ip, r->n_reps);
        r->n_reps++;
        if (svc->replicas[i].state != REPLICA_STATE_ACTIVE)
            continue;
        if (r->n_pts + (uint64_t)r->v > r->cap_pts &&
            !grow(g, (void **)&r->pts, &r->cap_pts, r->n_pts, r->n_pts + r->v, sizeof *r->pts))
            return 0;
        for (uint32_t k = 0; k < r->v; k++) {      /* initial build: all points at once, one sort */
            r->pts[r->n_pts].h = point_hash(seed, ip, k);
            r->pts[r->n_pts].ip = ip;
            r->n_pts++;
        }
    }
    qsort(r->pts, r->n_pts, sizeof *r->pts, point_cmp);
    return 1;
}

int ring_event(ring_t *r, int type, uint32_t ipv4)
{
    uint32_t id = ring_replica_id(r, ipv4);
    replica_t *rep = id == RING_NO_ID ? NULL : &r->reps[id];

    switch (type) {
    case REPLICA_ADD:
    case REPLICA_UP:
        if (rep && rep->state == REPLICA_STATE_ACTIVE)
            return 1;                              /* already active: nothing to do */
        /* Reserve everything first, then change: a failed gen_alloc leaves the ring as it was. */
        if (!grow(r->g, (void **)&r->pts, &r->cap_pts, r->n_pts, r->n_pts + r->v, sizeof *r->pts))
            return 0;
        if (!rep) {                                /* a new replica */
            if (!grow(r->g, (void **)&r->reps, &r->cap_reps, r->n_reps, r->n_reps + 1, sizeof *r->reps))
                return 0;
            if (!idmap_reserve(r, r->n_reps + 1))
                return 0;
            rep = &r->reps[r->n_reps];
            rep->ipv4 = ipv4;
            idmap_put(r->idmap, r->idmap_mask, ipv4, r->n_reps);
            r->n_reps++;
        }
        ring_add_points(r, ipv4);
        rep->state = REPLICA_STATE_ACTIVE;
        return 1;

    case REPLICA_DRAIN:
    case REPLICA_DOWN:
        if (!rep)
            return 1;                              /* unknown replica: nothing to remove */
        if (rep->state == REPLICA_STATE_ACTIVE)
            ring_remove_points(r, ipv4);
        rep->state = type == REPLICA_DRAIN ? REPLICA_STATE_DRAINING : REPLICA_STATE_DOWN;
        return 1;
    }
    return 1;
}

uint32_t ring_n_active(const ring_t *r)
{
    return r->n_pts / r->v;
}

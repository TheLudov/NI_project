/* ring.h: replica selection, a consistent-hash ring with virtual nodes (one ring per service).
 *
 * Author: Atieh El Gebrin
 *
 * Each ACTIVE replica owns v points on a 32-bit ring. A flow goes to the replica owning the first point
 * at or after the flow's hash (wrapping around). Draining and down replicas have no points, so they get no
 * new flows; flows already in the flow table keep their stored replica.
 *
 * The ring's content depends only on the set of active replicas (point positions depend only on
 * (seed, ip, k), and points are kept sorted by (h, ip)), never on the order of the events that led to it.
 * So the main LB and the harness's copy, which receive the same events, always hold the same ring.
 *
 * Replica events update the ring incrementally:
 *   - a replica leaving the active set: its v points are removed in one pass, O(P);
 *   - a replica joining the active set: its v points are generated, sorted, and merged in, O(P + v log v);
 * where P is the number of points on the ring. No full rebuild after the initial one.
 *
 * Memory: ring_init reserves room for the replica churn of the published scenarios (every initial replica
 * replaced once by a fresh address, plus a 25 % scale-out), so on_event does not allocate in those runs.
 * If it ever has to (more churn than that), it grows the arrays; should gen_alloc then fail on the main LB but
 * not on the copy (each has its own account), the two rings would differ, which is why we avoid it.
 *
 * Replica ids: every replica the service has known gets an id, its index in reps[]. Ids are stable for the
 * whole run (entries are never removed or moved), so the flow table can store a 16-bit id instead of a
 * 32-bit address, and check a stored replica's state in O(1) with ring_state().
 */
#ifndef RING_H
#define RING_H

#include <stdint.h>
#include "gen.h"

#define RING_NO_ID UINT32_MAX

typedef struct {
    uint32_t h;    /* position of the point on the ring */
    uint32_t ip;   /* the replica owning it (IPv4, host order) */
} ring_point_t;

typedef struct {
    gen_t   *g;           /* for allocations made later, in on_event */
    uint64_t seed;        /* secret seed: the point positions depend on it */
    uint32_t v;           /* points per active replica */

    replica_t *reps;      /* every replica the service has known, in any state; index = replica id */
    uint32_t n_reps, cap_reps;

    uint32_t *idmap;      /* open addressing, address -> id + 1 (0: empty); idmap_mask + 1 slots */
    uint32_t idmap_mask;

    ring_point_t *pts;    /* the points of the active replicas, sorted by (h, ip) */
    uint32_t n_pts, cap_pts;

    ring_point_t *scratch; /* v points: the new points of one replica, before the merge */
} ring_t;

/* Builds the ring of one service from its initial replicas. Returns 1 on success, 0 if gen_alloc fails. */
int ring_init(ring_t *r, gen_t *g, const service_t *svc, uint32_t v, uint64_t seed);

/* Applies a replica event (REPLICA_ADD, REPLICA_DRAIN, REPLICA_DOWN, REPLICA_UP).
 * Returns 1 on success, 0 if gen_alloc fails; the ring and the replica table are then left unchanged. */
int ring_event(ring_t *r, int type, uint32_t ipv4);

/* Number of active replicas (replicas with points on the ring). */
uint32_t ring_n_active(const ring_t *r);

/* The id of a replica known to the service, or RING_NO_ID. O(1). */
uint32_t ring_replica_id(const ring_t *r, uint32_t ipv4);

/* A replica's state (REPLICA_STATE_ACTIVE, _DRAINING, _DOWN) and address, by id. */
static inline uint8_t ring_state(const ring_t *r, uint32_t id) { return r->reps[id].state; }
static inline uint32_t ring_ip(const ring_t *r, uint32_t id) { return r->reps[id].ipv4; }

/* The replica of a flow hash: the owner of the first point with h >= hash, wrapping around.
 * Returns 0 if the service has no active replica. Branchless binary search (cmov at -O2). */
static inline uint32_t ring_lookup(const ring_t *r, uint32_t hash)
{
    uint32_t n = r->n_pts;
    if (n == 0)
        return 0;
    const ring_point_t *base = r->pts;
    while (n > 1) {
        uint32_t half = n / 2;
        base = (base[half].h < hash) ? base + half : base;
        n -= half;
    }
    uint32_t idx = (uint32_t)(base - r->pts) + (base->h < hash);
    if (idx == r->n_pts)
        idx = 0;                       /* past the last point: wrap to the first */
    return r->pts[idx].ip;
}

#endif /* RING_H */

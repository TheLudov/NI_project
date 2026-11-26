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
 * E4 (event cost): after ring_enable_coalescing, ring_event_defer only records the change (replica states change
 * at once, O(1)); ring_flush then applies every change recorded since the last flush together: one pass removes the points of all the
 * replicas that left, and the points of up to RING_CHUNK replicas that joined are sorted and merged in one pass.
 * A rolling upgrade's wave (dozens of replicas drained, or added, at the same instant) thus costs O(P) once
 * instead of once per replica. Without coalescing, ring_event works as before. The LB defers when
 * ev_coalesce=1 and flushes at the start of each batch's hash stage, before any ring_lookup: the main LB and the
 * harness's copy both do, so their rings stay the same.
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

    ring_point_t *scratch; /* v points (RING_CHUNK * v with coalescing): new points, before the merge */
    /* Changes recorded by ring_event_defer, applied by ring_flush (E4) */
    uint8_t  *pflags;     /* per replica id: RING_IN (its points are in pts), RING_ADD (waiting to be added) */
    uint32_t *rm_ips;     /* replicas whose points are in pts but must go (left the active set) */
    uint32_t n_rm;
    uint32_t *add_ids;    /* replicas whose points must be added (joined the active set) */
    uint32_t n_add;
    uint32_t *rm_set;     /* RING_RM_SET slots: rm_ips as an open-addressing set, for the removal pass */
    uint8_t  dirty;       /* something is recorded: ring_lookup needs a ring_flush first */
    uint8_t  coalesce;    /* ring_enable_coalescing was called */
} ring_t;

#define RING_CHUNK 16     /* replicas whose points are merged in one pass */
#define RING_RM_MAX 256   /* removals recorded before a flush is forced */
#define RING_ADD_MAX 256  /* additions recorded before a flush is forced */
#define RING_RM_SET 512   /* power of two, at least twice RING_RM_MAX */
#define RING_IN 1
#define RING_ADD 2

/* Builds the ring of one service from its initial replicas. Returns 1 on success, 0 if gen_alloc fails. */
int ring_init(ring_t *r, gen_t *g, const service_t *svc, uint32_t v, uint64_t seed);

/* Applies a replica event (REPLICA_ADD, REPLICA_DRAIN, REPLICA_DOWN, REPLICA_UP).
 * Returns 1 on success, 0 if gen_alloc fails; the ring and the replica table are then left unchanged. */
int ring_event(ring_t *r, int type, uint32_t ipv4);
/* E4: makes ring_event_defer record events instead of applying them (allocates the room: RING_CHUNK * v points
 * and a few small arrays). Returns 0 if gen_alloc fails (the ring then stays as it was). */
int ring_enable_coalescing(ring_t *r);
/* After ring_enable_coalescing: the same as ring_event, but the points only move at the next ring_flush: the
 * replica's state changes at once. Until then ring_lookup must not be called (r->dirty). Without coalescing it is
 * ring_event. */
int ring_event_defer(ring_t *r, int type, uint32_t ipv4);
/* Applies every change recorded since the last flush; nothing to do if !r->dirty. */
void ring_flush(ring_t *r);

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

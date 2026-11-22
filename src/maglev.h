//
// Created by belgae on 22/11/26.
//

#ifndef NI_PROJECT_MAGLEV_H
#define NI_PROJECT_MAGLEV_H
#include <stdint.h>
#include "gen.h"

typedef struct {
    replica_t r;                   /* ipv4 + REPLICA_STATE_* */
    uint32_t offset, skip;         /* the replica's permutation of the table */
} mg_replica_t;

typedef struct {
    uint32_t size;                 /* table size, prime */
    uint32_t n_replicas, n_active;
    mg_replica_t *replicas;
    uint32_t *next;                /* scratch for mg_populate: next slot of each replica */
    uint32_t *table;               /* size entries: a replica's IPv4, 0 = none */
} mg_service_t;

/* Builds svc from src's replicas (allocated with gen_alloc); returns 0 on failure. */
int  mg_init(gen_t *g, mg_service_t *svc, const service_t *src, uint32_t size, uint64_t seed);
/* Rebuilds the table from the active replicas. */
void mg_populate(mg_service_t *svc);

/* The replica of a flow hash, 0 if the service has no active replica. */
static inline uint32_t mg_lookup(const mg_service_t *svc, uint32_t hash)
{
    return svc->table[((uint64_t)hash * svc->size) >> 32];
}

#endif //NI_PROJECT_MAGLEV_H

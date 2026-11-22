//
// Created by belgae on 22/11/26.
//

#ifndef NI_PROJECT_MAGLEV_H
#define NI_PROJECT_MAGLEV_H
#include <stdint.h>

typedef struct {
    uint32_t ip;
    uint8_t state; /* REPLICA_STATE */
    uint32_t off, skip;
} mg_replica_t;

typedef struct {
    uint32_t n, cap;
    mg_replica_t *rep;
    uint16_t *table;
    uint32_t *next;
    uint32_t n_active;
} mg_service_t;

static void mg_populate(mg_service_t *sv, uint32_t M);

#endif //NI_PROJECT_MAGLEV_H

//
// Created by belgae on 22/11/26.
//

#include "maglev.h"

#include <string.h>
#include "crc32c.h"

int mg_init(gen_t *g, mg_service_t *svc, const service_t *src, uint32_t size, uint64_t seed)
{
    uint32_t n = src->n_replicas;
    svc->size = size;
    svc->n_replicas = n;
    svc->n_active = 0;
    svc->replicas = gen_alloc(g, (n ? n : 1) * sizeof *svc->replicas, 0, 0);
    svc->next = gen_alloc(g, (n ? n : 1) * sizeof *svc->next, 0, 0);
    svc->table = gen_alloc(g, size * sizeof *svc->table, 0, 0);
    if (!svc->replicas || !svc->next || !svc->table)
        return 0;

    uint32_t h0 = crc32c_u64(~0u, seed);
    for (uint32_t i = 0; i < n; i++) {
        mg_replica_t *rep = &svc->replicas[i];
        rep->r = src->replicas[i];
        uint32_t h1 = crc32c_u32(h0, rep->r.ipv4);
        uint32_t h2 = crc32c_u32(h1, rep->r.ipv4);
        rep->offset = h1 % size;
        rep->skip = h2 % (size - 1) + 1;
        svc->n_active += rep->r.state == REPLICA_STATE_ACTIVE;
    }
    mg_populate(svc);
    return 1;
}

void mg_populate(mg_service_t *svc)
{
    memset(svc->table, 0, svc->size * sizeof *svc->table);
    if (svc->n_active == 0)
        return;
    for (uint32_t i = 0; i < svc->n_replicas; i++)
        svc->next[i] = svc->replicas[i].offset;

    for (uint32_t filled = 0;;) {
        for (uint32_t i = 0; i < svc->n_replicas; i++) {
            const mg_replica_t *rep = &svc->replicas[i];
            if (rep->r.state != REPLICA_STATE_ACTIVE)
                continue;
            uint32_t slot;
            do {
                slot = svc->next[i];
                svc->next[i] = (svc->next[i] + rep->skip) % svc->size;
            } while (svc->table[slot]);
            svc->table[slot] = rep->r.ipv4;
            if (++filled == svc->size)
                return;
        }
    }
}

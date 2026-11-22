//
// Created by belgae on 22/11/26.
//

#include "maglev.h"

#include <string.h>
#include <gen.h>

static void mg_populate(mg_service_t *sv, uint32_t M) {
    if (sv->n_active == 0) return;
    memset(sv->table, 0xff, M * sizeof(sv->table));
    for (uint32_t i = 0; i < sv->n; i++) {
        sv->next[i] = 0;
    }

    uint32_t filled = 0;
    for (;;) {
        for (uint32_t i = 0; i < sv->n; i++) {
            if (sv->rep[i].state != REPLICA_STATE_ACTIVE) {
                continue;
            }
            uint32_t j;
            do {
                j = (sv->rep[i].off + sv->rep[i] * sv->rep[i].skip) % M;
                sv->next[i]++;
            } while (sv->table[j] != 0xffff);
            sv->table[j] = (uint16_t)i;
            if (++filled == M) {
                return;
            }
        }
    }
}

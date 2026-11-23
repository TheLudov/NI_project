/* tabhash.c: filling the tables of simple tabulation hashing (see tabhash.h).
 *
 * Author: Ludovic
 */
#include "tabhash.h"

/* splitmix64: a fast generator whose outputs are well mixed even for a small or structured seed. */
static uint64_t splitmix64(uint64_t *state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void tabhash_init(tabhash_t *th, uint64_t seed)
{
    uint64_t state = seed;
    for (int j = 0; j < TABHASH_KEY_BYTES; j++)
        for (int b = 0; b < 256; b++)
            th->t[j][b] = splitmix64(&state);
}

uint64_t tabhash_flow(const tabhash_t *th, const flow_key_t *k) {
    uint32_t s = k->src_ip, d = k->dst_ip;
    uint16_t sp = k->src_port, dp = k->dst_port;

    return th->t[0][s & 0xff] ^ th->t[1][(s >> 8) & 0xff] ^ th->t[2][(s >> 16) & 0xff] ^ th->t[3][s >> 24]
         ^ th->t[4][d & 0xff] ^ th->t[5][(d >> 8) & 0xff] ^ th->t[6][(d >> 16) & 0xff] ^ th->t[7][d >> 24]
         ^ th->t[8][sp & 0xff] ^ th->t[9][sp >> 8]
         ^ th->t[10][dp & 0xff] ^ th->t[11][dp >> 8]
         ^ th->t[12][k->protocol];
}

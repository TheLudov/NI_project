/* scripts/e2_hash/seedcheck.c: E2, do the collisions of a hash depend on its secret seed?
 *
 * 1. hash 2M random 5-tuples with seed A (111) and keep the low 32 bits (for CRC: both buckets and the signature);
 * 2. sort the hashes: equal hashes end up side by side, each such pair is a collision;
 * 3. hash both keys of every collision again with seed B (222): do they still collide?
 *
 * Expected: tabulation, almost none (new seed, new random tables); CRC32C, all of them. CRC is linear: changing its
 * initial value XORs the hash of every key of the same length with the same constant, so two keys that collide
 * collide for every seed, and an attacker can build such keys without knowing the seed.
 * Built and run by scripts/e2_hash.sh (step seed).
 *
 * Author: Ludovic
 */
#include <stdio.h>
#include <stdlib.h>

#include "tabhash.h"

#define N 2000000
#define SEED_A 111
#define SEED_B 222

static tabhash_t tables_a, tables_b;    /* the tabulation tables for seed A and seed B */

/* the low 32 bits of a key's hash: hf 0 = tabulation, 1 = CRC32C */
static uint32_t hash32(int hf, int seed_b, const flow_key_t *k)
{
    if (hf == 0)
        return (uint32_t)tabhash_flow(seed_b ? &tables_b : &tables_a, k);
    return (uint32_t)crc_flow(seed_b ? SEED_B : SEED_A, k);
}

typedef struct {
    uint32_t hash;
    uint32_t key;                       /* index of the key in the key array */
} hashed_t;

static int by_hash(const void *a, const void *b)
{
    uint32_t x = ((const hashed_t *)a)->hash, y = ((const hashed_t *)b)->hash;
    return x < y ? -1 : x > y;
}

static uint64_t rng_state = 42;
static uint64_t rnd64(void)             /* splitmix64 */
{
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

int main(void)
{
    flow_key_t *keys = calloc(N, sizeof *keys);
    hashed_t *h = malloc(N * sizeof *h);
    if (!keys || !h)
        return 1;
    tabhash_init(&tables_a, SEED_A);
    tabhash_init(&tables_b, SEED_B);

    /* random clients (address and port) towards the chat VIP, 198.51.100.10:443, TCP */
    for (int i = 0; i < N; i++) {
        uint64_t r = rnd64();
        keys[i] = (flow_key_t){ .src_ip = (uint32_t)(r >> 32), .src_port = (uint16_t)r,
                                .dst_ip = 0xC6336410u, .dst_port = 443, .protocol = 6 };
    }

    for (int hf = 0; hf < 2; hf++) {
        for (int i = 0; i < N; i++)
            h[i] = (hashed_t){ hash32(hf, 0, &keys[i]), (uint32_t)i };
        qsort(h, N, sizeof *h, by_hash);

        int pairs = 0, still = 0;
        for (int i = 1; i < N; i++) {
            if (h[i].hash != h[i - 1].hash)
                continue;
            pairs++;
            still += hash32(hf, 1, &keys[h[i].key]) == hash32(hf, 1, &keys[h[i - 1].key]);
        }
        printf("%s: %d colliding pairs under seed A, %d of them still collide under seed B\n",
               hf == 0 ? "tab" : "crc", pairs, still);
    }
    free(keys);
    free(h);
    return 0;
}

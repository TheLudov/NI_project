/* scripts/e2_hash/seedcheck.c: E2, do the collisions of a hash depend on its secret seed?
 *
 * Hashes 2M random 5-tuples with seed A, finds the pairs whose 32 low bits collide (what the index uses for CRC: both
 * buckets and the signature), and checks whether they still collide with seed B. CRC32C is linear: changing its initial
 * value XORs every hash of a fixed-length key with the same constant, so its collisions do not depend on the seed and
 * an attacker can craft them. Built and run by scripts/e2_hash.sh (step seed).
 *
 * Author: Ludovic
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tabhash.h"
typedef struct { uint32_t h; uint32_t i; } hi_t;
static int cmp(const void *a, const void *b) { uint32_t x = ((const hi_t *)a)->h, y = ((const hi_t *)b)->h; return x < y ? -1 : x > y; }
static uint64_t st = 42;
static uint64_t rnd(void) { uint64_t z = (st += 0x9E3779B97F4A7C15ull); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
int main(void) {
    enum { N = 2000000 };
    flow_key_t *k = calloc(N, sizeof *k); hi_t *v = malloc(N * sizeof *v);
    static tabhash_t ta, tb; tabhash_init(&ta, 111); tabhash_init(&tb, 222);
    for (int i = 0; i < N; i++) { uint64_t r = rnd(); k[i].src_ip = (uint32_t)(r >> 32); k[i].src_port = (uint16_t)r; k[i].dst_ip = 0xC6336410u; k[i].dst_port = 443; k[i].protocol = 6; }
    for (int hf = 0; hf < 2; hf++) {
        for (int i = 0; i < N; i++) { v[i].h = (uint32_t)(hf ? crc_flow(111, &k[i]) : tabhash_flow(&ta, &k[i])); v[i].i = (uint32_t)i; }
        qsort(v, N, sizeof *v, cmp);
        int pairs = 0, kept = 0;
        for (int i = 1; i < N; i++) if (v[i].h == v[i - 1].h) {
            pairs++;
            const flow_key_t *a = &k[v[i].i], *b = &k[v[i - 1].i];
            uint32_t ha = (uint32_t)(hf ? crc_flow(222, a) : tabhash_flow(&tb, a)), hb = (uint32_t)(hf ? crc_flow(222, b) : tabhash_flow(&tb, b));
            kept += ha == hb;
        }
        printf("%s: %d colliding pairs under seed A, %d of them still collide under seed B\n", hf ? "crc" : "tab", pairs, kept);
    }
    return 0;
}

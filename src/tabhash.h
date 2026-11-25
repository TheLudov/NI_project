/* tabhash.h: simple tabulation hashing of a flow's 5-tuple (brief, item 3 and E2).
 *
 * The key is the 13 bytes of the 5-tuple (src_ip 4, dst_ip 4, src_port 2, dst_port 2, protocol 1). Each key byte
 * has its own table of 256 random 64-bit values, filled from the secret seed at init; the hash is the XOR of the
 * 13 looked-up values. 13 x 256 x 8 B = 26 KB of tables.
 *
 * The 64-bit result is meant to give both bucket indices and the signature of the cuckoo index, from disjoint bit
 * ranges. The tables live in the LB's state (gen_alloc, no globals); both instances (main and copy) get the same
 * seed, so they build the same tables and agree on every hash.
 *
 * Author: Ludovic, and samuel for compute_hash() function
 */
#ifndef NI_PROJECT_TABHASH_H
#define NI_PROJECT_TABHASH_H

#include <stdint.h>

#include "parser.h"
#include "crc32c.h"

#define TABHASH_KEY_BYTES 13

/* Flow hash function, chosen with the param hashfn=tab (default) or hashfn=crc (the E2 baseline). */
enum { HASH_TAB, HASH_CRC };

typedef struct {
    uint64_t t[TABHASH_KEY_BYTES][256];
} tabhash_t;

/* Fills the tables with a pseudo-random stream derived from seed (deterministic: same seed, same tables). */
void tabhash_init(tabhash_t *th, uint64_t seed);

/* Hash of a 5-tuple: byte j of the key indexes table j, the 13 values are XORed. */
uint64_t tabhash_flow(const tabhash_t *th, const flow_key_t *k);

/*
function to compute the hash of a flow key based on the selected hash function (tabulation or CRC32C).
Parameters:
- hashfn: the hash function to use (HASH_TAB or HASH_CRC)
- th: pointer to the tabulation hash structure
- seed: the seed for initializing the hash tables
- key: pointer to the flow key
Return: the computed hash value
*/
uint64_t compute_hash(uint8_t hashfn, const tabhash_t *th, uint64_t seed, const flow_key_t *key);

/*
Function to compute the CRC32C hash of a flow key.
Parameters:
- seed: the seed for initializing the hash table
- key: pointer to the flow key
Return: the computed CRC32C hash value
*/
static inline uint64_t crc_flow(uint64_t seed,const flow_key_t *key);

#endif /* NI_PROJECT_TABHASH_H */

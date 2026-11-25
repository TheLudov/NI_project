/*Create and done By Samuel*/
#ifndef FLOW_TABLE_H
#define FLOW_TABLE_H

#include "parser.h"
#include "gen.h"
#include "tabhash.h"
#include "crc32c.h"

#define FT_NO_ENTRY UINT32_MAX // end of the free list
#define FT_MIN_SIG_BITS 8 // fewest signature bits a slot may keep (2^(32-8) - 1 entries at most)

//struct to represent a flow entry in the flow table (one element of the entry array)
//field order: 32 bytes (two entries per cache line) instead of 40 with the key first
//An entry is in use iff exactly one slot of the index refers to it; a free entry is on the free list, its replica
//field then holding the index of the next free entry. Entries never move once stored.
typedef struct {
    uint64_t expiration_time; // expiration time for the flow entry (trace time, ns)
    flow_key_t key; //id of the connexion
    uint32_t replica;//choosen server for the flow: its replica id in the service's ring (ring_replica_id)
    uint8_t state; //state of the flow: half-open, established, closing (lb/main.c)
}flow_entry_t;

//struct to represent the flow table: a cuckoo index (slots) and an entry array (entries), brief item 2
//A slot is one uint32_t: ((entry index + 1) << sig_bits) | signature, and 0 means an empty slot.
typedef struct {
    uint32_t *slots; // cuckoo index: num_buckets * bucket_size slots, bucket after bucket
    flow_entry_t *entries; // entry array: capacity entries
    uint32_t capacity; // number of entries = number of slots, so a free slot always has a free entry
    uint32_t free_head; // first free entry (FT_NO_ENTRY: none)
    uint8_t sig_bits; // bits of the signature in a slot; the other bits hold the entry index + 1

    uint32_t num_buckets; // number of buckets in the flow table
    uint32_t bucket_size; // number of slots in a bucket
    uint32_t max_depth; // Maximum depth for DFS search (displacements per insert)
    uint32_t dfs_budget; // Maximum entries examined by one insert's displacement search (both buckets)
    uint8_t hashfn; // hash function used for the flow table
    const tabhash_t *th; // pointer to the tabulation hash structure
    uint64_t seed; // seed for initializing the hash tables
}flow_table_t;



/*Function of initialization
Param:
g: pointer to the generator
num_buckets: size of the flow table
bucket_size: number of entries in the flow table
max_depth: Maximum depth for DFS search (displacements per insert)
dfs_budget: Maximum entries examined by one insert's displacement search (both buckets)
hashfn: hash function used for the flow table
th: pointer to the hash table
seed: seed used for the hash
Return: Pointer to the initialized flow table or NULL if the initialization fails (also when the table is too large
        for a slot to keep at least FT_MIN_SIG_BITS signature bits, i.e. num_buckets * bucket_size >= 2^24)
*/
flow_table_t *ft_init(gen_t *g, uint32_t num_buckets, uint32_t bucket_size, uint32_t max_depth, uint32_t dfs_budget, uint8_t hashfn, const tabhash_t *th, uint64_t seed);
// hash function used for the flow table
/*Function of look_up
Param:
ft: pointer to the flow table
key: pointer to the flow key
hash: hash value of the flow key
current_time: current time in nanoseconds
Only slots whose signature matches the key's are checked against the full 5-tuple in the entry array.
Return: pointer to the live entry of the flow, or NULL if the flow is not in the table. An expired entry of the flow
        is freed. Entries never move: the pointer stays valid until the entry expires and is reclaimed.
*/
flow_entry_t *ft_find(flow_table_t *ft, const flow_key_t *key, uint64_t hash, uint64_t current_time);


/*Function to insert or update a flow entry (upsert)
Param:
ft: pointer to the flow table
key: pointer to the flow key
replica: replica id to be stored
state: state of the flow
hash: hash value of the flow key
expiration_time: expiration time for the flow entry in nanoseconds
current_time: current time in nanoseconds: entries expired at this time count as free slots
The entry already holding the key is overwritten; otherwise an empty slot of the key's two buckets is used, then a
slot whose entry expired; otherwise slots are displaced (at most max_depth moves, dfs_budget slots examined). Only
the slots of the index move: entries stay where they are.
Return: 1 if the entry is stored, 0 otherwise
*/
int ft_insert(flow_table_t *ft, const flow_key_t *key, uint32_t replica, uint8_t state, uint64_t hash,
              uint64_t expiration_time, uint64_t current_time);


#endif // FLOW_TABLE_H
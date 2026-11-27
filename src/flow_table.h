/* Cuckoo flow table.
 * Responsible: Samuel (the table, insert, displacement search, expiry) and Ludovic (slots, entries, ft_init,
 * ft_find_at, ft_occupancy).
 *
 * Atieh El Gebrin's work in this table (flow_table.h and flow_table.c):
 *   - ft_loc_t and ft_locate: a key's two buckets and signature, computed once per packet in the LB's hash stage,
 *     then reused by ft_find_at, ft_insert_at and the prefetches (ft_find and ft_insert are ft_locate + these).
 *   - Faster bucket indices: multiply-shift (reduce, get_bucket_indices) instead of three divisions (~9 ns -> ~2 ns
 *     a key on our VM), and the automatic signature width (sig_bits=0).
 *   - Prefetching (E1): ft_prefetch_buckets and ft_prefetch_entries (pf_bucket=2: -13 % time per packet).
 *   - Insert fallback (E4): ft_evict_insert_at, policies 1-3 (ins_fallback).
 */
#ifndef FLOW_TABLE_H
#define FLOW_TABLE_H

#include "parser.h"
#include "gen.h"
#include "tabhash.h"
#include "crc32c.h"

#define FT_NO_ENTRY UINT32_MAX // end of the free list
#define FT_MIN_SIG_BITS 8 // fewest signature bits the automatic width (sig_bits = 0) may give (2^(32-8) - 1 entries at most)
#define FT_MAX_SIG_BITS 31 // most signature bits: at least one bit is needed for the entry index


// 1: ft_find counts its signature comparisons in ft->stats
// 0 (default, the Makefile's build): no counting, nothing added to the lookup loop.
#ifndef FT_STATS
#define FT_STATS 0
#endif

/* Flow states, kept in flow_entry_t.state (brief, item 2: half-open, established, closing). */
enum { FS_HALF_OPEN, FS_ESTABLISHED, FS_CLOSING };

/*
 * Service timeouts for different flow states.
 */
typedef struct {
    uint64_t half;
    uint64_t est;
    uint64_t fin;
} svc_timeouts_t;

//counters of ft_find (only with FT_STATS)
typedef struct {
    uint64_t lookups; // ft_find calls
    uint64_t hits; // of those, a live entry returned
    uint64_t slots_checked; // non-empty slots whose signature was compared with the key's
    uint64_t sig_matches; // of those, same signature: the entry array was read
    uint64_t false_matches; // of those, another flow's key (signature collision)
    uint32_t occupied_slots; //actual number of occupied slots in the flow table
    uint32_t first_failure_occupied; //number of occupied slots when the first insertion failure occurs
    uint8_t first_failure_seen; //flag to indicate if the first insertion failure has been seen

    uint64_t insertions_by_load[10]; //number of insertions at different load levels (0-9)
    uint64_t displacements_by_load[10]; //number of displacements at different load levels (0-9)
} ft_stats_t;

//struct to represent a flow entry in the flow table (one element of the entry array)
//field order: 32 bytes (two entries per cache line) instead of 40 with the key first
//An entry is in use iff exactly one slot of the index refers to it; a free entry is on the free list, its replica
//field then holding the index of the next free entry. Entries never move once stored.
typedef struct {
    uint64_t last_seen; // last time the flow was seen (trace time, ns); for a closing flow, the time its FIN or RST
                        // came: later packets do not move it, so they do not extend the grace period
    flow_key_t key; //id of the connexion
    uint32_t replica;//choosen server for the flow: its replica id in the service's ring (ring_replica_id)
    uint8_t state; //state of the flow: half-open, established, closing (lb/main.c)
    uint8_t service_id; //id of the service the flow belongs to (lb/main.c); fits in the padding: 32 bytes in all
}flow_entry_t;

//struct to represent the flow table: a cuckoo index (slots) and an entry array (entries)
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
    ft_stats_t stats; // ft_find's counters (stay 0 when FT_STATS is 0)
    const svc_timeouts_t *timeouts; // pointer to the service timeouts (lb/main.c)
    uint8_t insert_policy; // insertion policy: 0 for First available, 1 for least loaded, 2 for random
    uint64_t rng_state; // state of the random number generator for the random insertion policy
}flow_table_t;




/*Function of initialization
Param:
g: pointer to the generator
num_buckets: size of the flow table
bucket_size: number of entries in the flow table
max_depth: Maximum depth for DFS search (displacements per insert)
dfs_budget: Maximum entries examined by one insert's displacement search (both buckets)
sig_bits: signature width in bits (E1 sweep). 0: automatic, every bit the entry index leaves free (at least
          FT_MIN_SIG_BITS). Otherwise 1..32 - idx_bits, where idx_bits is the bits needed for num_buckets * bucket_size
hashfn: hash function used for the flow table
th: pointer to the hash table
seed: seed used for the hash
timeouts: pointer to the service timeouts (lb/main.c)
Return: Pointer to the initialized flow table or NULL if the initialization fails (also when sig_bits does not fit
        next to the entry index, or, with sig_bits = 0, when the table is too large for a slot to keep at least
        FT_MIN_SIG_BITS signature bits, i.e. num_buckets * bucket_size >= 2^24)
*/
flow_table_t *ft_init(gen_t *g, uint32_t num_buckets, uint32_t bucket_size, uint32_t max_depth, uint32_t dfs_budget,
                      uint8_t sig_bits, uint8_t hashfn, const tabhash_t *th, uint64_t seed, const svc_timeouts_t *timeouts);
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

/* Where a key can be in the index: its two buckets and its signature, as ft_find and ft_insert compute them from the
   hash (Atieh). ft_locate computes it once (the bucket indices cost three divisions); ft_find_at, ft_insert_at and
   the prefetches below reuse it. ft_find and ft_insert are ft_locate followed by these. */
typedef struct {
    uint32_t bucket[2];
    uint32_t sig;
} ft_loc_t;
void ft_locate(const flow_table_t *ft, uint64_t hash, ft_loc_t *loc);
flow_entry_t *ft_find_at(flow_table_t *ft, const flow_key_t *key, const ft_loc_t *loc, uint64_t current_time);


/*Function to insert or update a flow entry (upsert)
Param:
ft: pointer to the flow table
key: pointer to the flow key
replica: replica id to be stored
state: state of the flow
hash: hash value of the flow key
service_id: id of the service the flow belongs to
current_time: current time in nanoseconds: entries expired at this time count as free slots
The entry already holding the key is overwritten; otherwise an empty slot of the key's two buckets is used, then a
slot whose entry expired; otherwise slots are displaced (at most max_depth moves, dfs_budget slots examined). Only
the slots of the index move: entries stay where they are.
Return: 1 if the entry is stored, 0 otherwise
*/
int ft_insert(flow_table_t *ft, const flow_key_t *key, uint32_t replica, uint8_t state, uint64_t hash,
              uint32_t service_id, uint64_t current_time);
int ft_insert_at(flow_table_t *ft, const flow_key_t *key, uint32_t replica, uint8_t state, const ft_loc_t *loc,
                 uint32_t service_id, uint64_t current_time);

/* E4 (Atieh): the fallback after a failed ft_insert_at (lb/main.c, ins_fallback): an entry of the key's two buckets
   gives its slot to the key. policy 1: the entry seen longest ago; policy 2: a half-open entry first, then a closing
   one, then the entry seen longest ago; policy 3: the entry seen longest ago among the key's own service's. Return: 1
   if stored, 0 if no entry may give its slot up (policy 3: none of the key's service). */
int ft_evict_insert_at(flow_table_t *ft, const flow_key_t *key, uint32_t replica, uint8_t state,
                       const ft_loc_t *loc, uint32_t service_id, uint64_t current_time, int policy);
/* Prefetching (E1, by Atieh). These two only ask the CPU to start loading cache lines (__builtin_prefetch) and
   return at once, without waiting for them; they change nothing in the table. The LB's stages call them on a whole
   batch before they look the batch up (lb/main.c: pf_bucket, pf_entry), so that the cache misses of the batch's
   packets overlap instead of being paid one after the other. */

/* Starts loading the key's first bucket of the index, and its second one too if both (every cache line they span).
   A lookup that finds the key in its first bucket never reads the second, and most stored keys are in their first
   bucket (an insert tries it first): asking for the second costs a cache line, and a page walk, most hits never use. */
void ft_prefetch_buckets(const flow_table_t *ft, const ft_loc_t *loc, int both);

/* Starts loading the entries that the key's first bucket (both buckets if both) points to with the key's signature:
   normally only the key's own entry, if it is stored there. It reads the bucket(s), so it pays off once
   ft_prefetch_buckets has brought them in. */
void ft_prefetch_entries(const flow_table_t *ft, const ft_loc_t *loc, int both);

/* E5 occupancy probe (Ludovic), measurement only: walks the whole index (slow: never call it in a timed run).
   live[3 * s + state] receives the live entries of service s in each state (FS_HALF_OPEN, FS_ESTABLISHED,
   FS_CLOSING), for s < n_svc (at most FT_OCC_MAX_SVC); *expired the entries still in the index whose timeout has
   passed at now (not yet reclaimed: an insert or a lookup frees them lazily). */
#define FT_OCC_MAX_SVC 16
void ft_occupancy(const flow_table_t *ft, uint64_t now, uint32_t n_svc, uint64_t *live, uint64_t *expired);

#endif // FLOW_TABLE_H
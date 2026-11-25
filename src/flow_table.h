/*Create and done By Samuel*/
#ifndef FLOW_TABLE_H
#define FLOW_TABLE_H

#define BUCKET_SIZE 10 // number of entries in the flow table

#include "parser.h"
#include "gen.h"
#include "tabhash.h"
#include "crc32c.h"


//struct to represent a flow entry in the flow table
//field order: 32 bytes (two entries per cache line) instead of 40 with the key first
typedef struct {
    uint64_t expiration_time; // expiration time for the flow entry (trace time, ns)
    flow_key_t key; //id of the connexion
    uint32_t replica;//choosen server for the flow: its replica id in the service's ring (ring_replica_id)
    uint8_t state; //state of the flow: half-open, established, closing (lb/main.c)
    uint8_t occupied; //indicates if the entry is occupied or not
}flow_entry_t;

//struct to represent a bucket in the flow table
typedef struct{
    flow_entry_t entries[BUCKET_SIZE];
}flow_bucket_t;

//struct to represent the flow table
typedef struct {
    flow_bucket_t *buckets; // array of buckets
    uint32_t num_buckets; // number of buckets in the flow table
    uint8_t hashfn; // hash function used for the flow table
    const tabhash_t *th; // pointer to the tabulation hash structure
    uint64_t seed; // seed for initializing the hash tables
}flow_table_t;



/*Function of initialization
Param:
g: pointer to the generator
Bucket_size: size of the flow table

Return: Pointer to the initialized flow table or NULL if the initialization fails
*/
flow_table_t *ft_init(gen_t *g, uint32_t Bucket_number, uint8_t hashfn, const tabhash_t *th, uint64_t seed);

/*Function of look_up
Param:
ft: pointer to the flow table
key: pointer to the flow key
hash: hash value of the flow key
current_time: current time in nanoseconds
Return: pointer to the live entry of the flow (the caller may read and update it until the next ft_insert,
        which can move entries), or NULL if the flow is not in the table. An expired entry of the flow is freed.
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
The slot already holding the key is overwritten; otherwise a free or expired slot of the key's two buckets is
used; otherwise entries are displaced (at most MAX_DEPTH moves, FT_DFS_BUDGET entries examined).
Return: 1 if the entry is stored, 0 otherwise
*/
int ft_insert(flow_table_t *ft, const flow_key_t *key, uint32_t replica, uint8_t state, uint64_t hash,
              uint64_t expiration_time, uint64_t current_time);


#endif // FLOW_TABLE_H
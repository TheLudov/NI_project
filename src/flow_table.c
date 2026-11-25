/*Create and done By Samuel*/
#include "flow_table.h"

#include <stddef.h>

typedef struct {
    uint32_t bucket_index;
    int entry_index;
} dfs_state_t;

static int same_flow_key(const flow_key_t *key1, const flow_key_t *key2);
static int find_empty_slot(flow_table_t *ft, uint32_t bucket_index, uint64_t current_time);
static int is_in_path(dfs_state_t *path, int depth, uint32_t bucket_index);
static int ft_cuuko_DFS(flow_table_t *ft, dfs_state_t *path, uint32_t bucket_index, uint32_t depth, int *path_depth,
                        uint32_t *free_bucket, uint32_t *free_slot, uint64_t current_time, int *budget);
static void get_bucket_indices(flow_table_t *ft, uint64_t hash, uint32_t *bucket1, uint32_t *bucket2);

static inline flow_entry_t *bucket_at(const flow_table_t *ft, uint32_t bucket_index) {
    return ft->entries + (size_t)bucket_index * ft->bucket_size;
}


flow_table_t *ft_init(gen_t *g, uint32_t num_buckets, uint32_t bucket_size, uint32_t max_depth, uint32_t dfs_budget, uint8_t hashfn, const tabhash_t *th, uint64_t seed)
{
    // check if valide value and no overflow of size_t
    if(num_buckets <2 || bucket_size == 0 || max_depth == 0 || (size_t)num_buckets > SIZE_MAX / sizeof(flow_entry_t) / bucket_size)
        return NULL;

    flow_table_t *ft = (flow_table_t *) gen_alloc(g, sizeof(flow_table_t), 0, 0);

    if(ft == NULL)
        return NULL;


    ft->num_buckets = num_buckets;
    ft->bucket_size = bucket_size;
    ft->max_depth = max_depth;
    ft->dfs_budget = dfs_budget;
    ft->hashfn = hashfn;
    ft->th = th;
    ft->seed = seed;
    ft->entries = (flow_entry_t *) gen_alloc(g, (size_t)num_buckets * bucket_size * sizeof(flow_entry_t), 0, 0);

    if(ft->entries == NULL)
    {
        gen_mfree(g, ft);
        return NULL;
    }

    for(uint32_t i = 0; i < num_buckets; i++)
    {
        for(uint32_t j = 0; j<ft->bucket_size; j++)
        {
            bucket_at(ft, i)[j].occupied = 0;
            bucket_at(ft, i)[j].expiration_time = 0;
            bucket_at(ft, i)[j].replica = 0;
            bucket_at(ft, i)[j].state = 0;
        }
    }
    return ft;
}



/* An entry is free for an insert if it is empty or has expired (expired entries are reclaimed lazily). */
static inline int slot_free(const flow_entry_t *e, uint64_t now)
{
    return !e->occupied || e->expiration_time <= now;
}

static inline void write_entry(flow_entry_t *e, const flow_key_t *key, uint32_t replica, uint8_t state,
                               uint64_t expiration_time)
{
    e->key = *key;
    e->replica = replica;
    e->state = state;
    e->expiration_time = expiration_time;
    e->occupied = 1;
}

flow_entry_t *ft_find(flow_table_t *ft, const flow_key_t *key, uint64_t hash, uint64_t current_time){

    uint32_t buckets[2];
    get_bucket_indices(ft, hash, &buckets[0], &buckets[1]);

    //check the two buckets of the key; a key is stored at most once
    for(int b = 0; b < 2; b++)
    {
        for(uint32_t i = 0; i< ft->bucket_size; i++)
        {
            flow_entry_t *e = &bucket_at(ft, buckets[b])[i];
            if(e->occupied && same_flow_key(&e->key, key))
            {
                if(e->expiration_time > current_time)
                    return e;
                e->occupied = 0; //expired: free the slot
                return NULL;
            }
        }
    }
    return NULL;
}

int ft_insert(flow_table_t *ft, const flow_key_t *key, uint32_t replica, uint8_t state, uint64_t hash,
              uint64_t expiration_time, uint64_t current_time){

    uint32_t buckets[2];
    get_bucket_indices(ft, hash, &buckets[0], &buckets[1]);

    //the key already stored (live or expired): update it in place, never store it twice
    //otherwise remember the first free or expired slot, bucket1 first
    flow_entry_t *free_entry = NULL;
    for(int b = 0; b < 2; b++)
    {
        for(uint32_t i = 0; i<ft->bucket_size; i++)
        {
            flow_entry_t *e = &bucket_at(ft, buckets[b])[i];
            if(e->occupied && same_flow_key(&e->key, key))
            {
                write_entry(e, key, replica, state, expiration_time);
                return 1;
            }
            if(!free_entry && slot_free(e, current_time))
                free_entry = e;
        }
    }
    if(free_entry)
    {
        write_entry(free_entry, key, replica, state, expiration_time);
        return 1;
    }

    //both buckets full of live entries: displace entries (cuckoo), within a bounded search
    dfs_state_t path[ft->max_depth];

    int path_depth = 0;
    uint32_t free_bucket = 0;
    uint32_t free_slot = 0;
    int budget = /*FT_DFS_BUDGET*/ft->dfs_budget;
    int found = ft_cuuko_DFS(ft, path, buckets[0], 0, &path_depth, &free_bucket, &free_slot, current_time, &budget);

    if (!found)
    {
        found = ft_cuuko_DFS(ft, path, buckets[1], 0, &path_depth, &free_bucket, &free_slot, current_time, &budget);
    }

    if (!found)
        return 0;

    for(int i = path_depth -1; i>=0; i--){
        uint32_t src_bucket = path[i].bucket_index;
        int src_slot = path[i].entry_index;

        bucket_at(ft, free_bucket)[free_slot] = bucket_at(ft, src_bucket)[src_slot];

        free_bucket = src_bucket;
        free_slot = src_slot;
    }

    write_entry(&bucket_at(ft, free_bucket)[free_slot], key, replica, state, expiration_time);
    return 1;
}



/*
Function to compare two flows
Param:
key1: pointer to the first flow key
key2: pointer to the second flow key
Return: 1 if the two flow keys are the same, 0 otherwise
*/
static int same_flow_key(const flow_key_t *key1, const flow_key_t *key2)
{
    return (key1->src_ip == key2->src_ip) &&
           (key1->dst_ip == key2->dst_ip) &&
           (key1->src_port == key2->src_port) &&
           (key1->dst_port == key2->dst_port) &&
           (key1->protocol == key2->protocol);
}

/*
Function to find a free slot in a bucket: empty, or holding an expired entry
Param:
ft: pointer to the flow table
bucket_index: index of the bucket to search for an empty slot
current_time: current time in nanoseconds
Return: index of the free slot if found, -1 otherwise
*/
static int find_empty_slot(flow_table_t *ft, uint32_t bucket_index, uint64_t current_time)
{
    for (uint32_t i = 0; i < ft->bucket_size; i++)
    {
        if (slot_free(&bucket_at(ft, bucket_index)[i], current_time))
        {
            return i; // Return the index of the free slot
        }
    }
    return -1; // No free slot found
}

/*
Function to check if a slot is in the current path
Param:
path: array of DFS states
depth: current depth of the search
bucket_index: index of the bucket to check
Return: 1 if the slot is in the path, 0 otherwise
*/
static int is_in_path(dfs_state_t *path, int depth, uint32_t bucket_index)
{
    for (int i = 0; i < depth; i++)
    {
        if (path[i].bucket_index == bucket_index)
        {
            return 1; // Found in path
        }
    }
    return 0; // Not found in path
}

/*
Function to perform a depth-first search for a free slot in the flow table
Param:
ft: pointer to the flow table
bucket_index: index of the current bucket to search
depth: current depth of the search
current_time: current time in nanoseconds (expired entries count as free)
budget: entries the search may still examine; each one costs a hash, so a full table costs at most this much
Return: 1 if a free slot is found, 0 otherwise
*/
static int ft_cuuko_DFS(flow_table_t *ft,dfs_state_t *path, uint32_t bucket_index, uint32_t depth,int *path_depth,
                        uint32_t *free_bucket, uint32_t *free_slot, uint64_t current_time, int *budget){

    int slot_index = find_empty_slot(ft, bucket_index, current_time);
    if(slot_index != -1){
        *path_depth = depth;
        *free_bucket = bucket_index;
        *free_slot = slot_index;
        return 1; // Found a free slot
    }

    if(depth >= ft->max_depth)
        return 0;

    for(uint32_t i = 0; i < ft->bucket_size; i++){
        if(*budget <= 0)
            return 0; // search budget spent: give up
        (*budget)--;

        flow_entry_t *entry = &bucket_at(ft,bucket_index)[i];

        uint64_t hash = compute_hash(ft->hashfn, ft->th, ft->seed, &entry->key);

        uint32_t bucket1, bucket2;

        get_bucket_indices(ft, hash, &bucket1, &bucket2);

        uint32_t other_bucket;
        if (bucket_index == bucket1)
            other_bucket = bucket2;
        else if (bucket_index == bucket2)
            other_bucket = bucket1;
        else
            continue;

        path[depth].bucket_index = bucket_index;
        path[depth].entry_index = i;

        if(!is_in_path(path, depth+1, other_bucket)){
            if(ft_cuuko_DFS(ft, path, other_bucket, depth + 1,path_depth, free_bucket, free_slot, current_time, budget)){
                return 1;
            }
        }
    }

    return 0; // No free slot found in this path

}

/*
Fnction to get the two bucket indices for a given hash value
Param:
ft: pointer to the flow table
hash: hash value of the flow key
bucket1: pointer to store the first bucket index
bucket2: pointer to store the second bucket index
Return: void
*/
static void get_bucket_indices(flow_table_t *ft, uint64_t hash, uint32_t *bucket1, uint32_t *bucket2){

    if(ft->hashfn == HASH_TAB){
        *bucket1 = (uint32_t) hash % ft->num_buckets;
        *bucket2 = (uint32_t)((*bucket1 + 1 + (hash >> 32) % (ft->num_buckets - 1)) % ft->num_buckets);
    }
    else{
       *bucket1 = (uint32_t) hash % ft->num_buckets;

        uint32_t hash2 = crc32c_u64( ~0u, ((uint64_t)hash << 32) | hash);

        *bucket2 = (*bucket1 + 1 + hash2 % (ft->num_buckets - 1)) % ft->num_buckets;
    }
}
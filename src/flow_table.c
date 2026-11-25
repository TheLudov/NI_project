/*Create and done By Samuel*/
#include "flow_table.h"
#define MAX_DEPTH 5 // Maximum depth for DFS search

typedef struct {
    uint32_t bucket_index;
    int entry_index;
} dfs_state_t;

int same_flow_key(flow_key_t *key1, flow_key_t *key2);
int find_empty_slot(flow_table_t *ft, uint32_t bucket_index, uint64_t current_time);
int is_in_path(dfs_state_t *path, int depth, uint32_t bucket_index);
int ft_cuuko_DFS(flow_table_t *ft, dfs_state_t *path, uint32_t bucket_index, int depth, int *path_depth, uint32_t *free_bucket, uint32_t *free_slot, uint64_t current_time);
static void get_bucket_indices(flow_table_t *ft, uint64_t hash, uint32_t *bucket1, uint32_t *bucket2);
void copy_flow_entry(flow_table_t *ft,flow_key_t *key, uint32_t replica, uint64_t expiration_time, uint32_t bucket_index, int entry_index);
static flow_entry_t *find_flow(flow_table_t *ft, flow_key_t *key,uint64_t hash, uint64_t current_time);


flow_table_t *ft_init(gen_t *g, uint32_t Bucket_number, uint8_t hashfn, const tabhash_t *th, uint64_t seed)
{
    if(Bucket_number <2)
        return NULL;

    flow_table_t *ft = (flow_table_t *) gen_alloc(g, sizeof(flow_table_t), 0, 0);

    if(ft == NULL)
        return NULL;


    ft->num_buckets = Bucket_number;
    ft->hashfn = hashfn;
    ft->th = th;
    ft->seed = seed;
    ft->buckets = (flow_bucket_t *) gen_alloc(g, sizeof(flow_bucket_t) * Bucket_number, 0, 0);

    if(ft->buckets == NULL)
    {
        gen_mfree(g, ft);
        return NULL;
    }

    for(int i = 0; i< Bucket_number; i++)
    {
        for(int j = 0; j<BUCKET_SIZE; j++)
        {
            ft->buckets[i].entries[j].occupied = 0;
            ft->buckets[i].entries[j].expiration_time = 0;
            ft->buckets[i].entries[j].replica = 0;
            ft->buckets[i].entries[j].state = 0;
        }
    }
    return ft;
}



int ft_lookup(flow_table_t *ft, flow_key_t *key, uint32_t *replica, uint64_t hash, uint64_t current_time){

    flow_entry_t *entry = find_flow(ft, key, hash, current_time);

    if (entry == NULL)
        return 0;

    *replica = entry->replica;
    return 1;

}

int ft_insert(flow_table_t *ft, flow_key_t *key, uint32_t replica,uint64_t hash, uint64_t expiration_time, uint64_t current_time){

    uint32_t bucket1, bucket2;
    get_bucket_indices(ft, hash, &bucket1, &bucket2);

    flow_entry_t *entry = find_flow(ft, key, hash, current_time);

    if (entry != NULL)
    {
        entry->replica = replica;
        entry->expiration_time = expiration_time;
        return 1;
    }

    int finded_slot = -1;

    finded_slot = find_empty_slot(ft, bucket1, current_time);
    if(finded_slot != -1){
        
        copy_flow_entry(ft, key, replica, expiration_time, bucket1, finded_slot);
        return 1;
    }
    else {
        finded_slot = find_empty_slot(ft, bucket2, current_time);
        if(finded_slot != -1){
            copy_flow_entry(ft, key, replica, expiration_time, bucket2, finded_slot);
            return 1;
        }
    }

    dfs_state_t path[MAX_DEPTH];

    int path_depth = 0;
    uint32_t free_bucket = 0;
    uint32_t free_slot = 0;
    int found = ft_cuuko_DFS(ft, path, bucket1, 0, &path_depth, &free_bucket, &free_slot, current_time);

    if (!found)
    {
        found = ft_cuuko_DFS(ft, path, bucket2, 0,&path_depth, &free_bucket, &free_slot, current_time);
    }

    if (!found)
        return 0;

    for(int i = path_depth -1; i>=0; i--){
        uint32_t src_bucket = path[i].bucket_index;
        int src_slot = path[i].entry_index;

        ft->buckets[free_bucket].entries[free_slot] =
        ft->buckets[src_bucket].entries[src_slot];

        free_bucket = src_bucket;
        free_slot = src_slot;
    }
    
    copy_flow_entry(ft, key, replica, expiration_time, free_bucket, free_slot);
    return 1;
}

/*
Function to find a flow entry in the flow table based on the flow key and hash value.
Param:
ft: pointer to the flow table
key: pointer to the flow key
hash: hash value of the flow key
current_time: current time in nanoseconds
Return: pointer to the found flow entry, or NULL if not found
*/
static flow_entry_t *find_flow(flow_table_t *ft, flow_key_t *key,uint64_t hash, uint64_t current_time)
{
    uint32_t bucket1, bucket2;
    get_bucket_indices(ft, hash, &bucket1, &bucket2);

    uint32_t buckets[2] = {bucket1, bucket2};

    for (int b = 0; b < 2; b++)
    {
        for (int i = 0; i < BUCKET_SIZE; i++)
        {
            flow_entry_t *entry = &ft->buckets[buckets[b]].entries[i];

            if (entry->occupied && same_flow_key(&entry->key, key))
            {
                if (entry->expiration_time > current_time)
                {
                    return entry;
                }
                else
                {
                    entry->occupied = 0;
                }
            }
        }
    }

    return NULL;
}
/*
Function to copy a flow entry from one bucket to another
Param:
ft: pointer to the flow table
key: pointer to the flow key
replica: replica value to be copied
expiration_time: expiration time for the flow entry in nanoseconds
bucket_index: index of the bucket to copy to
entry_index: index of the entry in the bucket to copy
Return: void
*/
void copy_flow_entry(flow_table_t *ft,flow_key_t *key, uint32_t replica, uint64_t expiration_time, uint32_t bucket_index, int entry_index){
    
    ft->buckets[bucket_index].entries[entry_index].key = *key;
    ft->buckets[bucket_index].entries[entry_index].replica = replica;
    ft->buckets[bucket_index].entries[entry_index].expiration_time = expiration_time;
    ft->buckets[bucket_index].entries[entry_index].occupied = 1;
    ft->buckets[bucket_index].entries[entry_index].state = 0;
}

/*
Function to compare two flows
Param:
key1: pointer to the first flow key
key2: pointer to the second flow key
Return: 1 if the two flow keys are the same, 0 otherwise
*/
int same_flow_key(flow_key_t *key1, flow_key_t *key2)
{
    return (key1->src_ip == key2->src_ip) &&
           (key1->dst_ip == key2->dst_ip) &&
           (key1->src_port == key2->src_port) &&
           (key1->dst_port == key2->dst_port) &&
           (key1->protocol == key2->protocol);
}

/*
Function to find an empty slot in a bucket
Param:
ft: pointer to the flow table
bucket_index: index of the bucket to search for an empty slot
Return: index of the empty slot if found, -1 otherwise
*/
int find_empty_slot(flow_table_t *ft, uint32_t bucket_index, uint64_t current_time)
{
    for (int i = 0; i < BUCKET_SIZE; i++)
    {
        if (!ft->buckets[bucket_index].entries[i].occupied)
        {
            return i; // Return the index of the empty slot
        }
        if(ft->buckets[bucket_index].entries[i].expiration_time <= current_time)
        {
            ft->buckets[bucket_index].entries[i].occupied = 0; // Mark the slot as unoccupied
            return i; // Return the index of the now empty slot
        }
    }
    return -1; // No empty slot found
}

/*
Function to check if a slot is in the current path
Param:
path: array of DFS states
depth: current depth of the search
bucket_index: index of the bucket to check
Return: 1 if the slot is in the path, 0 otherwise
*/
int is_in_path(dfs_state_t *path, int depth, uint32_t bucket_index)
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
Function to perform a depth-first search for an empty slot in the flow table
Param:
ft: pointer to the flow table
bucket_index: index of the current bucket to search
depth: current depth of the search
Return: 1 if an empty slot is found, 0 otherwise
*/
int ft_cuuko_DFS(flow_table_t *ft,dfs_state_t *path, uint32_t bucket_index, int depth,int *path_depth, uint32_t *free_bucket, uint32_t *free_slot,uint64_t current_time){

    int slot_index = find_empty_slot(ft, bucket_index, current_time);
    if(slot_index != -1){
        *path_depth = depth;
        *free_bucket = bucket_index;
        *free_slot = slot_index;
        return 1; // Found an empty slot
    }

    if(depth >= MAX_DEPTH)
        return 0;

    for(int i = 0; i < BUCKET_SIZE; i++){
        flow_entry_t *entry = &ft->buckets[bucket_index].entries[i];

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
            if(ft_cuuko_DFS(ft, path, other_bucket, depth + 1,path_depth, free_bucket, free_slot, current_time)){
                return 1;
            }
        }

        

        
    } 
    
    return 0; // No empty slot found in this path

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
/*Create and done By Samuel*/
#include "flow_table.h"
#define MAX_DEPTH 5 // Maximum depth for DFS search

typedef struct {
    uint32_t bucket_index;
    int entry_index;
} dfs_state_t;

int same_flow_key(flow_key_t *key1, flow_key_t *key2);
int find_empty_slot(flow_table_t *ft, uint32_t bucket_index);
int is_in_path(dfs_state_t *path, int depth, uint32_t bucket_index);
int ft_cuuko_DFS(flow_table_t *ft, dfs_state_t *path, uint32_t bucket_index, int depth, int *path_depth, uint32_t *free_bucket, uint32_t *free_slot);
static void get_bucket_indices(flow_table_t *ft, uint64_t hash, uint32_t *bucket1, uint32_t *bucket2);


flow_table_t *ft_init(gen_t *g, uint32_t Bucket_number, uint8_t hashfn)
{
    if(Bucket_number <2)
        return NULL;

    flow_table_t *ft = (flow_table_t *) gen_alloc(g, sizeof(flow_table_t), 0, 0);

    if(ft == NULL)
        return NULL;


    ft->num_buckets = Bucket_number;
    ft->hashfn = hashfn;
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

    uint32_t bucket1, bucket2;
    get_bucket_indices(ft, hash, &bucket1, &bucket2);

    //check the slot
    //fist bucket
    for(int i = 0; i<BUCKET_SIZE; i++)
    {
        if(ft->buckets[bucket1].entries[i].occupied && same_flow_key(&ft->buckets[bucket1].entries[i].key, key))
        {
            if(ft->buckets[bucket1].entries[i].expiration_time > current_time)
            {
                *replica = ft->buckets[bucket1].entries[i].replica;
                return 1;
            }
            else
            {
                ft->buckets[bucket1].entries[i].occupied = 0;
                continue;
            }
        }
    }
    //second bucket
    for(int i = 0; i<BUCKET_SIZE; i++)
    {
        if(ft->buckets[bucket2].entries[i].occupied && same_flow_key(&ft->buckets[bucket2].entries[i].key, key))
        {
            if(ft->buckets[bucket2].entries[i].expiration_time > current_time)
            {
                *replica = ft->buckets[bucket2].entries[i].replica;
                return 1;
            }
            else
            {
                ft->buckets[bucket2].entries[i].occupied = 0;
                return 0;
            }
        }
    }
    return 0;
}

int ft_insert(flow_table_t *ft, flow_key_t *key, uint32_t replica,uint64_t hash, uint64_t expiration_time, uint64_t current_time){

    uint32_t bucket1, bucket2;
    get_bucket_indices(ft, hash, &bucket1, &bucket2);

    for(int i = 0; i<BUCKET_SIZE; i++)
    {
        if(!ft->buckets[bucket1].entries[i].occupied)
        {
            ft->buckets[bucket1].entries[i].key = *key;
            ft->buckets[bucket1].entries[i].replica = replica;
            ft->buckets[bucket1].entries[i].expiration_time = expiration_time;
            ft->buckets[bucket1].entries[i].occupied = 1;
            return 1;
        }
    }
    for(int i = 0; i<BUCKET_SIZE; i++)
    {
        if(!ft->buckets[bucket2].entries[i].occupied)
        {
            ft->buckets[bucket2].entries[i].key = *key;
            ft->buckets[bucket2].entries[i].replica = replica;
            ft->buckets[bucket2].entries[i].expiration_time = expiration_time;
            ft->buckets[bucket2].entries[i].occupied = 1;
            return 1;
        }
    }
    dfs_state_t path[MAX_DEPTH];

    int path_depth = 0;
    uint32_t free_bucket = 0;
    uint32_t free_slot = 0;
    int found = ft_cuuko_DFS(ft, path, bucket1, 0, &path_depth, &free_bucket, &free_slot);

    if (!found)
    {
        found = ft_cuuko_DFS(ft, path, bucket2, 0,&path_depth, &free_bucket, &free_slot);
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
    
    ft->buckets[free_bucket].entries[free_slot].key = *key;
    ft->buckets[free_bucket].entries[free_slot].replica = replica;
    ft->buckets[free_bucket].entries[free_slot].expiration_time = expiration_time;
    ft->buckets[free_bucket].entries[free_slot].occupied = 1;
    return 1;
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
int find_empty_slot(flow_table_t *ft, uint32_t bucket_index)
{
    for (int i = 0; i < BUCKET_SIZE; i++)
    {
        if (!ft->buckets[bucket_index].entries[i].occupied)
        {
            return i; // Return the index of the empty slot
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
int ft_cuuko_DFS(flow_table_t *ft,dfs_state_t *path, uint32_t bucket_index, int depth,int *path_depth, uint32_t *free_bucket, uint32_t *free_slot){

    int slot_index = find_empty_slot(ft, bucket_index);
    if(slot_index != -1){
        *path_depth = depth;
        *free_bucket = bucket_index;
        *free_slot = slot_index;
        return 1; // Found an empty slot
    }

    if(depth >= MAX_DEPTH)
        return 0;

    for(int i = 0; i < BUCKET_SIZE; i++){
        //hash 
        flow_entry_t *entry = &ft->buckets[bucket_index].entries[i];
        uint32_t hash  =0; //faire fonction de hashage pour le flow_key 
        uint32_t bucket1 = hash % ft->num_buckets;

        uint32_t hash2 = crc32c_u64(
            ~0u,
            ((uint64_t)hash << 32) | hash
        );

        uint32_t bucket2 = (bucket1 + 1 + hash2 % (ft->num_buckets - 1)) % ft->num_buckets;

        uint32_t other_bucket;
        if(bucket_index == bucket1)
            other_bucket = bucket2;
        else
            other_bucket = bucket1;

        path[depth].bucket_index = bucket_index;
        path[depth].entry_index = i;

        if(!is_in_path(path, depth+1, other_bucket)){
            if(ft_cuuko_DFS(ft, path, other_bucket, depth + 1,path_depth, free_bucket, free_slot)){
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
       *bucket1 = hash % ft->num_buckets;

        uint32_t hash2 = crc32c_u64( ~0u, ((uint64_t)hash << 32) | hash);

        *bucket2 = (*bucket1 + 1 + hash2 % (ft->num_buckets - 1)) % ft->num_buckets;
    }
}
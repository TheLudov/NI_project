/*Create and done By Samuel*/
#include "flow_table.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    uint32_t bucket_index;
    int entry_index;
} dfs_state_t;

static int same_flow_key(const flow_key_t *key1, const flow_key_t *key2);
static int find_expired_slot(flow_table_t *ft, uint32_t bucket_index, uint64_t current_time);
static int find_empty_slot(flow_table_t *ft, uint32_t bucket_index, uint64_t current_time);
static int is_in_path(dfs_state_t *path, int depth, uint32_t bucket_index);
static int ft_cuuko_DFS(flow_table_t *ft, dfs_state_t *path, uint32_t bucket_index, uint32_t depth, int *path_depth, uint32_t *free_bucket, uint32_t *free_slot, uint64_t current_time, int *budget);
static void get_bucket_indices(const flow_table_t *ft, uint64_t hash, uint32_t *bucket1, uint32_t *bucket2);
static int is_expired(const flow_table_t *ft,const flow_entry_t *entry,uint64_t current_time);

/* ---- the cuckoo index: slots ---- */

/* The slots of one bucket */
static inline uint32_t *bucket_at(const flow_table_t *ft, uint32_t bucket_index) {
    return ft->slots + (size_t)bucket_index * ft->bucket_size;
}

/* The signature of a key: the low sig_bits bits of the low 32 bits of its hash. Two keys may share one: the full
 * key decides. Low half, because crc_flow gives only 32 bits (the top of the 64 would always be 0) and because
 * tabulation's second bucket uses the high half (get_bucket_indices). The first bucket is (lo * num_buckets) >> 32,
 * which depends on the top log2(num_buckets) bits of the same 32 (Atieh: multiply-shift instead of a modulo), so the
 * two are disjoint when log2(num_buckets) + sig_bits <= 32, which the automatic width (sig_bits = 0) guarantees. */
static inline uint32_t sig_of(const flow_table_t *ft, uint64_t hash) {
    return (uint32_t)hash & ((1u << ft->sig_bits) - 1);
}

/* Maps x, uniform over 32 bits, onto [0, n): (x * n) >> 32 spreads it like x % n would, with a multiplication instead
 * of a division (Atieh: the three divisions of the two bucket indices cost ~9 ns a key on our VM, this ~2 ns). */
static inline uint32_t reduce(uint32_t x, uint32_t n) {
    return (uint32_t)(((uint64_t)x * n) >> 32);
}

/* The signature held in a non-empty slot */
static inline uint32_t slot_sig(const flow_table_t *ft, uint32_t slot) {
    return slot & ((1u << ft->sig_bits) - 1);
}

/* The entry index held in a non-empty slot (stored + 1, so that 0 is an empty slot) */
static inline uint32_t slot_idx(const flow_table_t *ft, uint32_t slot) {
    return (slot >> ft->sig_bits) - 1;
}

static inline uint32_t make_slot(const flow_table_t *ft, uint32_t sig, uint32_t idx) {
    return ((idx + 1) << ft->sig_bits) | sig;
}

/* The entry a non-empty slot refers to */
static inline flow_entry_t *slot_entry(const flow_table_t *ft, uint32_t slot) {
    return &ft->entries[slot_idx(ft, slot)];
}

/* ---- the entry array: free list ---- */

/* Takes a free entry. Never fails while a slot is free: there are as many entries as slots. */
static inline uint32_t entry_alloc(flow_table_t *ft) {
    uint32_t idx = ft->free_head;
    ft->free_head = ft->entries[idx].replica;
    return idx;
}

static inline void entry_free(flow_table_t *ft, uint32_t idx) {
    ft->entries[idx].replica = ft->free_head;
    ft->free_head = idx;
}

/* Empties a slot, returning its entry (if any) to the free list */
static inline void slot_release(flow_table_t *ft, uint32_t *slot) {
    if (*slot) {
        entry_free(ft, slot_idx(ft, *slot));
        *slot = 0;
    }
}


flow_table_t *ft_init(gen_t *g, uint32_t num_buckets, uint32_t bucket_size, uint32_t max_depth, uint32_t dfs_budget,
                      uint8_t sig_bits, uint8_t hashfn, const tabhash_t *th, uint64_t seed, const svc_timeouts_t *timeouts)
{
    // check if valide value
    if(num_buckets <2 || bucket_size == 0 || max_depth == 0 || sig_bits > FT_MAX_SIG_BITS)
        return NULL;
    uint64_t capacity = (uint64_t)num_buckets * bucket_size;

    // index bits: enough for capacity (an entry index + 1 is at most capacity); the signature takes at most the rest
    uint32_t idx_bits = 1;
    while (((1ull << idx_bits) - 1) < capacity)
        idx_bits++;
    if (idx_bits > 32 - 1)
        return NULL;
    uint32_t max_sig = 32 - idx_bits;
    if (sig_bits == 0) { // automatic: every free bit, but no fewer than FT_MIN_SIG_BITS
        if (max_sig < FT_MIN_SIG_BITS)
            return NULL;
        sig_bits = (uint8_t)max_sig;
    } else if (sig_bits > max_sig) {
        return NULL;
    }

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
    ft->capacity = (uint32_t)capacity;
    ft->sig_bits = sig_bits;
    memset(&ft->stats, 0, sizeof ft->stats);
    ft->timeouts = timeouts;
    // 64-byte aligned, so that a bucket of 16 slots is exactly one cache line
    ft->slots = (uint32_t *) gen_alloc(g, (size_t)capacity * sizeof(uint32_t), 64, 0);
    ft->entries = (flow_entry_t *) gen_alloc(g, (size_t)capacity * sizeof(flow_entry_t), 64, 0);

    if(ft->slots == NULL || ft->entries == NULL)
    {
        if (ft->slots)
            gen_mfree(g, ft->slots);
        if (ft->entries)
            gen_mfree(g, ft->entries);
        gen_mfree(g, ft);
        return NULL;
    }

    // every slot empty, every entry on the free list (in index order)
    memset(ft->slots, 0, (size_t)capacity * sizeof(uint32_t));
    memset(ft->entries, 0, (size_t)capacity * sizeof(flow_entry_t));
    for(uint32_t i = 0; i < ft->capacity; i++)
        ft->entries[i].replica = i + 1 < ft->capacity ? i + 1 : FT_NO_ENTRY;
    ft->free_head = 0;
    return ft;
}



static inline void write_entry(flow_entry_t *e, const flow_key_t *key, uint32_t replica, uint8_t state, uint64_t service_id, uint64_t current_time)
{
    e->key = *key;
    e->replica = replica;
    e->state = state;
    e->last_seen = current_time;
    e->service_id = service_id;
}

/* Where a key can be: its two buckets and its signature (Atieh: computed once per packet, in the LB's hash stage,
   then reused by the prefetches, the lookup and the insert; the bucket indices cost three divisions) */
void ft_locate(const flow_table_t *ft, uint64_t hash, ft_loc_t *loc)
{
    get_bucket_indices(ft, hash, &loc->bucket[0], &loc->bucket[1]);
    loc->sig = sig_of(ft, hash);
}

flow_entry_t *ft_find(flow_table_t *ft, const flow_key_t *key, uint64_t hash, uint64_t current_time)
{
    ft_loc_t loc;
    ft_locate(ft, hash, &loc);
    return ft_find_at(ft, key, &loc, current_time);
}

flow_entry_t *ft_find_at(flow_table_t *ft, const flow_key_t *key, const ft_loc_t *loc, uint64_t current_time){

    const uint32_t *buckets = loc->bucket;
    uint32_t sig = loc->sig;
#if FT_STATS
    ft->stats.lookups++;
#endif

    //check the two buckets of the key; a key is stored at most once
    //the entry array is read only on a signature match
    for(int b = 0; b < 2; b++)
    {
        uint32_t *bucket = bucket_at(ft, buckets[b]);
        for(uint32_t i = 0; i< ft->bucket_size; i++)
        {
            uint32_t s = bucket[i];
            if(s == 0)
                continue;
#if FT_STATS
            ft->stats.slots_checked++;
#endif
            if(slot_sig(ft, s) != sig)
                continue;
#if FT_STATS
            ft->stats.sig_matches++;
#endif
            flow_entry_t *e = slot_entry(ft, s);
            if(!same_flow_key(&e->key, key)) {
#if FT_STATS
                ft->stats.false_matches++;
#endif
                continue; //same signature, other flow
            }
            if(!is_expired(ft, e, current_time)) {
#if FT_STATS
                ft->stats.hits++;
#endif
                return e;
            }
            slot_release(ft, &bucket[i]); //expired: free the slot and its entry
            return NULL;
        }
    }
    return NULL;
}

/* ---- prefetching (E1), by Atieh ---- */

/* Asks the CPU to start loading every cache line of [p, p + bytes) and returns at once, without waiting */
static inline void prefetch_range(const void *p, size_t bytes)
{
    const char *line = (const char *)((uintptr_t)p & ~(uintptr_t)63);
    const char *end = (const char *)p + bytes;
    for (; line < end; line += 64)
        __builtin_prefetch(line);
}

void ft_prefetch_buckets(const flow_table_t *ft, const ft_loc_t *loc, int both)
{
    prefetch_range(bucket_at(ft, loc->bucket[0]), ft->bucket_size * sizeof(uint32_t));
    if (both)
        prefetch_range(bucket_at(ft, loc->bucket[1]), ft->bucket_size * sizeof(uint32_t));
}

void ft_prefetch_entries(const flow_table_t *ft, const ft_loc_t *loc, int both)
{
    for (int b = 0; b < (both ? 2 : 1); b++) {
        const uint32_t *bucket = bucket_at(ft, loc->bucket[b]);
        for (uint32_t i = 0; i < ft->bucket_size; i++) {
            uint32_t s = bucket[i];
            if (s != 0 && slot_sig(ft, s) == loc->sig)
                prefetch_range(slot_entry(ft, s), sizeof(flow_entry_t)); /* an entry may span two lines */
        }
    }
}

int ft_insert(flow_table_t *ft, const flow_key_t *key, uint32_t replica, uint8_t state, uint64_t hash,
              uint32_t service_id, uint64_t current_time)
{
    ft_loc_t loc;
    ft_locate(ft, hash, &loc);
    return ft_insert_at(ft, key, replica, state, &loc, service_id, current_time);
}

int ft_insert_at(flow_table_t *ft, const flow_key_t *key, uint32_t replica, uint8_t state, const ft_loc_t *loc,
                 uint32_t service_id, uint64_t current_time){

    const uint32_t *buckets = loc->bucket;
    uint32_t sig = loc->sig;

    //the key already stored (live or expired): update its entry in place, never store it twice
    //otherwise remember the first empty slot, bucket1 first (the index alone tells it, no entry is read)
    uint32_t *target = NULL;
    for(int b = 0; b < 2; b++)
    {
        uint32_t *bucket = bucket_at(ft, buckets[b]);
        for(uint32_t i = 0; i<ft->bucket_size; i++)
        {
            uint32_t s = bucket[i];
            if(s == 0)
            {
                if(!target)
                    target = &bucket[i];
                continue;
            }
            if(slot_sig(ft, s) != sig)
                continue;
            flow_entry_t *e = slot_entry(ft, s);
            if(same_flow_key(&e->key, key))
            {
                write_entry(e, key, replica, state, service_id, current_time);
                return 1;
            }
        }
    }

    //no empty slot: a slot whose entry has expired (this reads the entries of the two buckets)
    for(int b = 0; b < 2 && !target; b++)
    {
        int i = find_expired_slot(ft, buckets[b], current_time);
        if(i >= 0)
            target = &bucket_at(ft, buckets[b])[i];
    }

    if(!target)
    {
        //both buckets full of live entries: displace slots (cuckoo), within a bounded search
        dfs_state_t path[ft->max_depth];

        int path_depth = 0;
        uint32_t free_bucket = 0;
        uint32_t free_slot = 0;
        int budget = ft->dfs_budget;
        int found = ft_cuuko_DFS(ft, path, buckets[0], 0, &path_depth, &free_bucket, &free_slot, current_time, &budget);

        if (!found)
        {
            found = ft_cuuko_DFS(ft, path, buckets[1], 0, &path_depth, &free_bucket, &free_slot, current_time, &budget);
        }

        if (!found)
            return 0;

        //the free slot found may hold an expired entry: free it before a slot is moved onto it
        slot_release(ft, &bucket_at(ft, free_bucket)[free_slot]);

        //move the slots along the path; the entries they refer to stay where they are
        for(int i = path_depth -1; i>=0; i--){
            uint32_t src_bucket = path[i].bucket_index;
            int src_slot = path[i].entry_index;

            bucket_at(ft, free_bucket)[free_slot] = bucket_at(ft, src_bucket)[src_slot];

            free_bucket = src_bucket;
            free_slot = src_slot;
        }
        //the first slot of the path still holds a copy of the slot moved out of it: clear it, do not release it
        //(its entry now belongs to the moved slot)
        target = &bucket_at(ft, free_bucket)[free_slot];
        *target = 0;
    }

    //store: free the expired entry the slot may hold first, so that its entry can be the one taken
    slot_release(ft, target);
    uint32_t idx = entry_alloc(ft);
    write_entry(&ft->entries[idx], key, replica, state, service_id, current_time);
    *target = make_slot(ft, sig, idx);
    return 1;
}


/* ---- internal functions ---- */
/*
Function to check if an entry has expired
Param:
ft: pointer to the flow table
entry: pointer to the flow entry
current_time: current time in nanoseconds
Return: 1 if the entry has expired, 0 otherwise
*/
static int is_expired(const flow_table_t *ft,
                      const flow_entry_t *entry,
                      uint64_t current_time)
{
    const svc_timeouts_t *to = &ft->timeouts[entry->service_id];

    uint64_t timeout;
    uint64_t reference_time = entry->last_seen;

    switch (entry->state) {
        case FS_HALF_OPEN:
            timeout = to->half;
            break;

        case FS_ESTABLISHED:
            timeout = to->est;
            break;

        case FS_CLOSING:
            timeout = to->fin;   /* from the FIN or RST: last_seen is not refreshed once closing */
            break;

        default:
            return 1;
    }

    if (current_time < reference_time)
        return 0;

    return (current_time - reference_time) >= timeout;
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
Function to find a slot whose entry has expired in a bucket (expired entries are reclaimed lazily)
Param:
ft: pointer to the flow table
bucket_index: index of the bucket to search
current_time: current time in nanoseconds
Return: index of the slot if found, -1 otherwise
*/
static int find_expired_slot(flow_table_t *ft, uint32_t bucket_index, uint64_t current_time)
{
    uint32_t *bucket = bucket_at(ft, bucket_index);
    for (uint32_t i = 0; i < ft->bucket_size; i++)
    {
        if (bucket[i] != 0 && is_expired(ft, slot_entry(ft, bucket[i]), current_time))
            return i;
    }
    return -1;
}

/*
Function to find a free slot in a bucket: an empty one first (index only), else one whose entry has expired
Param:
ft: pointer to the flow table
bucket_index: index of the bucket to search for an empty slot
current_time: current time in nanoseconds
Return: index of the free slot if found, -1 otherwise. An expired entry is not freed here: the caller does it
        when it actually uses the slot.
*/
static int find_empty_slot(flow_table_t *ft, uint32_t bucket_index, uint64_t current_time)
{
    uint32_t *bucket = bucket_at(ft, bucket_index);
    for (uint32_t i = 0; i < ft->bucket_size; i++)
    {
        if (bucket[i] == 0)
        {
            return i; // Return the index of the free slot
        }
    }
    return find_expired_slot(ft, bucket_index, current_time);
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
budget: slots the search may still examine; each one costs a hash, so a full table costs at most this much
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

        //the bucket is full (find_empty_slot failed): every slot refers to a live entry, whose key gives its
        //other bucket; the signature is too short to give it, so the key is hashed again
        flow_entry_t *entry = slot_entry(ft, bucket_at(ft, bucket_index)[i]);

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
static void get_bucket_indices(const flow_table_t *ft, uint64_t hash, uint32_t *bucket1, uint32_t *bucket2){

    // the second bucket's offset: tabulation's high 32 bits; crc_flow has only 32 bits, so a second CRC of them
    uint32_t hi = ft->hashfn == HASH_TAB ? (uint32_t)(hash >> 32)
                                         : crc32c_u64(~0u, ((uint64_t)hash << 32) | hash);
    uint32_t b1 = reduce((uint32_t)hash, ft->num_buckets);
    uint32_t b2 = b1 + 1 + reduce(hi, ft->num_buckets - 1); // 1 .. num_buckets - 1 after b1: never b1 itself
    if (b2 >= ft->num_buckets)
        b2 -= ft->num_buckets;
    *bucket1 = b1;
    *bucket2 = b2;
}

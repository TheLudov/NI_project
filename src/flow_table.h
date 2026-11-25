/*Create and done By Samuel*/
#ifndef FLOW_TABLE_H
#define FLOW_TABLE_H

#define BUCKET_SIZE 10 // number of entries in the flow table

#include "parser.h"
#include "gen.h"
#include "tabhash.h"
#include "crc32c.h"


//struct to represent a flow entry in the flow table
typedef struct {
    flow_key_t key; //id of the connexion
    uint32_t replica;//choosen server for the flow
    uint64_t expiration_time; // expiration time for the flow entry
    uint8_t state; //state  of the flow (TCP/UDP)
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
}flow_table_t;



/*Function of initialization
Param:
g: pointer to the generator
Bucket_size: size of the flow table

Return: Pointer to the initialized flow table or NULL if the initialization fails
*/
flow_table_t *ft_init(gen_t *g, uint32_t Bucket_number, uint8_t hashfn);

/*Function of look_up
Param:
ft: pointer to the flow table
key: pointer to the flow key
replica: pointer to the replica variable
hash: hash value of the flow key
current_time: current time in nanoseconds
Return: 1 if the flow is found, 0 otherwise
*/
int ft_lookup(flow_table_t *ft, flow_key_t *key, uint32_t *replica, uint64_t hash, uint64_t current_time);


/*Function to insert a new flow entry
Param:
ft: pointer to the flow table
key: pointer to the flow key
replica: replica value to be inserted
hash: hash value of the flow key
expiration_time: expiration time for the flow entry in nanoseconds
Return: 1 if the insertion is successful, 0 otherwise
*/
int ft_insert(flow_table_t *ft, flow_key_t *key, uint32_t replica,uint64_t hash, uint64_t expiration_time, uint64_t current_time);


#endif // FLOW_TABLE_H
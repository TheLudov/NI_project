/* gen.h: the INFO8011 L4 load balancer harness, public API.
 *
 * The harness plays your LB's two NICs, generates the traffic, checks every
 * forwarding decision and measures everything.  Your program calls gen_open,
 * then loops: gen_rx_batch, your stages (through the gen_* wrappers),
 * gen_tx_batch / gen_drop, until gen_running returns 0.
 *
 * Threads: in saturation and paced modes the harness calls the second copy of
 * your LB (see init) from its own generator thread.  Keep all your state in
 * your lb_t (no mutable globals) and the two copies never interfere.
 */
#ifndef GEN_H
#define GEN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gen gen_t;

/* ---- what the harness gives your LB ---- */
#define GEN_HEADROOM 20
#define GEN_BUFSIZE  1536
typedef struct {
    uint64_t ts_ns;     /* trace timestamp: your LB's only clock */
    uint16_t off;       /* offset of the frame from the start of buf: GEN_HEADROOM on receive,
                           0 for a tunnelled reply (bonus) */
    uint16_t len;       /* bytes in the frame, Ethernet header included */
    uint8_t *buf;       /* a GEN_BUFSIZE-byte buffer: the headroom, then the frame */
} pkt_t;

typedef struct { uint32_t ipv4; uint8_t state; } replica_t;  /* state: active, draining, down */
enum { REPLICA_STATE_ACTIVE, REPLICA_STATE_DRAINING, REPLICA_STATE_DOWN };   /* replica_t.state */

typedef struct {
    uint32_t vip; uint16_t port; uint8_t proto;    /* one service */
    uint32_t n_replicas; const replica_t *replicas;
} service_t;

/* Addresses (vip, ipv4, int_ipv4) and ports are in host byte order. */
typedef struct {
    uint32_t n_services; const service_t *services;
    uint8_t  ext_mac[6], int_mac[6];               /* the LB's external and internal NICs */
    uint8_t  border_mac[6], zone_mac[6];           /* the routers' interfaces facing the LB */
    uint32_t int_ipv4;                             /* the LB's internal address: the outer source */
    uint64_t seed;                                 /* secret, for the student's hash */
    size_t   mem_budget;                           /* bytes */
    const char *params;                            /* the whole params string given to gen_open */
} gen_config_t;

enum { GEN_NIC_EXT, GEN_NIC_INT };

typedef struct {
    uint64_t ts_ns;                                /* virtual time */
    enum { REPLICA_ADD, REPLICA_DRAIN, REPLICA_DOWN, REPLICA_UP } type;
    uint32_t service;                              /* index in cfg->services */
    uint32_t ipv4;                                 /* the replica, by its address */
} gen_event_t;

/* ---- what the LB registers ---- */
typedef struct lb lb_t;                            /* the student defines struct lb */
typedef struct pkt_state pkt_state_t;              /* the student defines struct pkt_state */

/* mask: a 64-bit bitmap over the packet array, bit i standing for p[i] (and s[i]);
   a batch holds at most 64 packets.
   As an argument, bit i set to 1 means: process p[i].
   As a return value, bit i set to 1 means: the stage succeeded on p[i].
   gen_tx_batch and gen_drop take one too: bit i set to 1 means send, or drop, p[i].
   Bits at or above n mean nothing, wherever a mask appears: the harness ignores them. */
typedef void     (*hash_fn) (lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask);
typedef uint64_t (*stage_fn)(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask);

/* init is called twice: the first result is your main LB, the second a copy
   that the harness uses to play the other LB instances of the tier.  The copy
   receives every replica event and, for flows that started elsewhere, a call
   to hash then compute_replica (mask 1, a zeroed state): compute_replica must
   therefore work without lookup having run. */
typedef struct {
    size_t   state_size;                           /* sizeof(pkt_state_t) */
    lb_t  *(*init)      (gen_t *g, const gen_config_t *cfg);   /* called twice */
    void   (*on_event)  (lb_t *lb, const gen_event_t *ev);
    hash_fn  hash;
    stage_fn lookup, compute_replica, insert;
    uint32_t (*replica_of)(const pkt_state_t *s);  /* the replica's IPv4 address compute_replica left in s */
} lb_ops_t;

/* ---- what the LB calls ---- */
gen_t      *gen_open(const char *scenario, const char *params, const lb_ops_t *ops);
const char *gen_param(const gen_t *g, const char *key);   /* a key's value in params, NULL if absent */
int         gen_running(const gen_t *g);
lb_t       *gen_lb(gen_t *g);                              /* the state init returned for the main LB */
uint8_t     gen_rx_batch(gen_t *g, int nic, pkt_t **pkts, uint8_t max);   /* never blocks, may return 0 */
void        gen_tx_batch(gen_t *g, int nic, pkt_t **pkts, uint8_t n, uint64_t mask);
void        gen_drop    (gen_t *g, pkt_t **pkts, uint8_t n, uint64_t mask);
/* gen_alloc: align 0 or 1 means alignof(max_align_t); NULL beyond mem_budget.  huge == 0: never backed by
   huge pages; huge != 0: backed by huge pages if the kernel can, counted in whole huge pages, aligned to one. */
void       *gen_alloc   (gen_t *g, size_t bytes, size_t align, int huge);
void        gen_mfree   (gen_t *g, void *ptr);

/* stage wrappers: same arguments as the stages, plus g; they call the student's
   function, then count (and, with profiling on, time) from the masks passed and returned */
void     gen_hash   (gen_t *g, lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask);
uint64_t gen_lookup (gen_t *g, lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask);
uint64_t gen_replica(gen_t *g, lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask);
uint64_t gen_insert (gen_t *g, lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask);

/* ---- results (call once at the end; also called automatically at exit) ---- */
void gen_close(gen_t *g);   /* writes the results, frees everything */

#ifdef __cplusplus
}
#endif
#endif /* GEN_H */

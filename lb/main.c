/* lb/main.c: the LB's main program: its stages (lb_ops_t) and its loop.
 *
 *   build/lb SCENARIO "PARAMS"
 *
 * runs your LB on SCENARIO with the parameters PARAMS (key=value,...), then
 * exits; the harness writes the results file (results=PATH, default
 * results.csv).  The contest runs
 *
 *   build/lb mixed_flood.scn "$(build/autotune),$(cat config.params),KEYS"
 *
 * KEYS being the harness keys of the run (mode, results, ...).  A key given
 * twice makes gen_open fail: exit with an error then, as below.
 *
 * Put your components (the stages, your tables) in src/: build/autotune links
 * them too.  README.md documents the harness.
 *
 * Who is responsible for what (each function below also names its owner):
 *   Ludovic: parameters and timeouts (param_ul, init_timeouts), the lookup and insert stages, encapsulation
 *            (ip_checksum, put32, encap), the main loop, the SYN-flood defence and occupancy probe (E5).
 *   Samuel:  the flow-table calls in init, hash, lookup and insert, and the parser's use in hash.
 *   Atieh:   replica selection (rings in init, on_event, compute_replica, the down-replica and SYN rules in
 *            lookup), prefetching (pf_*), ft_loc reuse, batch, ins_fallback and ev_coalesce (E4), and the
 *            E3/E4 statistics (e3_*, e4_*).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gen.h"
#include "parser.h"
#include "ring.h"
#include "flow_table.h"
#include "tabhash.h"

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_ACK 0x10


/* A connection's first packet: SYN set, ACK not set (a SYN-ACK never reaches the LB: replies bypass it). */
static inline int is_syn(uint8_t f) { return (f & (TCP_SYN | TCP_ACK)) == TCP_SYN; }



/* E4 (Atieh): -DE4_STATS=1 prints, once per second of trace time, each service's table entries by state and its
   misses, new flows, failed inserts and evictions (scripts/e4/run_e4.sh). Off by default: nothing changes in the
   normal build. */
#ifndef E4_STATS
#define E4_STATS 0
#endif

struct lb {
    gen_config_t cfg; // copy of gen_config_t
    ring_t *rings;    /* one consistent-hash ring per service, cfg.services[k] -> rings[k] (Atieh) */
    flow_table_t *ft; /* cuckoo flow table (Samuel) */
    tabhash_t *th;    /* simple tabulation hashing tables, filled from cfg.seed */
    uint8_t hashfn;   /* HASH_TAB or HASH_CRC */
    svc_timeouts_t *to; /* timeouts per service, indexed by service_id */
    /* Prefetching (E1, Atieh): where the stages ask the CPU to load cache lines ahead, each 0 (off) or on:
       pf_pkt=1 the packet headers hash parses; pf_bucket=1 the first bucket of the index lookup reads, =2 both
       buckets; pf_entry=1 the entries the first bucket points to with the key's signature, =2 those of both
       buckets. And how far ahead, pf_dist: 0 = a stage first asks for its whole batch, then works on it; D > 0 =
       while it works on packet i it asks for packet i + D (buckets for i + 2D when pf_entry needs them first), so
       that at most a few packets' lines are on their way at once. */
    uint8_t pf_pkt, pf_bucket, pf_entry;
    uint32_t pf_dist;
    /* E4 (Atieh): batch = packets taken from the receive ring at a time (1..64, default 64); ins_fallback = what a
       failed insert does: 0 (default) forward the flow by consistent hashing without an entry, 1 evict the entry
       of the key's buckets seen longest ago, 2 evict a half-open one first, then a closing one, then the oldest,
       3 evict the oldest of the key's own service (ft_evict_insert_at). ins_fallback_<VIP> sets it for one service
       (E4 found that the best one differs). */
    uint8_t batch;
    uint8_t fallback[16];     /* ins_fallback per service, by service id (at most 15 services) */
    /* E4 (Atieh, event cost): ev_coalesce=1 records replica events (ring_event_defer) and applies them together at
       the start of the next batch's hash stage (ring_flush), so a wave of events costs one pass over the ring
       instead of one per event. 0 (default): each event is applied at once. */
    uint8_t ev_coalesce;
#if E4_STATS
    uint64_t e4_evict[16]; /* evictions per service (E4_STATS builds only) */
#endif
    /* E5, SYN-flood defence: syn_insert=1 (default) stores a half-open entry for every SYN; syn_insert=0 forwards a
       SYN by consistent hashing without storing it. The flow's next packet (the ACK) then misses, consistent
       hashing gives it the same replica (same hash, same ring), and it is stored established. Spoofed SYNs that
       are never completed thus take no slot of the shared table. */
    uint8_t syn_insert;
    /* E5, adaptive variant: syn_guard=R (SYNs per second, 0 = off) lets each service store at most R/10 SYNs per
       100 ms window of trace time; the service's further SYNs in that window are forwarded without an entry, as
       with syn_insert=0. Normal traffic stays below the limit (web opens ~10k flows/s), so only a flooded service
       goes stateless, and only during the flood: a replica event between a SYN and its ACK then cannot break the
       other services' flows. */
    uint32_t syn_guard_win;     /* SYNs a service may store per window (0: no guard) */
    uint64_t *guard_start;      /* per service: start of its current window (trace ns) */
    uint32_t *guard_count;      /* per service: SYNs stored in it */
    /* E5, occupancy probe (measurement only): every occ_every_s seconds of trace time, the live entries per
       service and state are appended to occ_file. 0: off. */
    uint64_t occ_every_ns, occ_next_ns;
    FILE *occ;
};

/* pf_pkt (Atieh): the 54 bytes of Ethernet + IPv4 + TCP headers from off, which may span two cache lines */
static inline void prefetch_headers(const pkt_t *pk)
{
    const uint8_t *f = pk->buf + pk->off;
    __builtin_prefetch(f);
    __builtin_prefetch(f + 53);
}
struct pkt_state {
    flow_key_t key;
    uint64_t hash;
    ft_loc_t loc;   /* the key's buckets and signature, from the hash, computed once in hash() (Atieh) */
    uint32_t replica;
    uint32_t service_id;
    uint8_t tcp_flags;
    uint8_t valid;
};


/* State of a flow inserted on this packet: a SYN opens it, a FIN or RST closes it, anything else is a flow
   already in progress (from another LB, or evicted earlier). UDP has no handshake: established. */
/* Responsible: Ludovic */
static uint8_t first_state(const pkt_state_t *s)
{
    if (s->key.protocol != 6)
        return FS_ESTABLISHED;
    if (s->tcp_flags & (TCP_FIN | TCP_RST))
        return FS_CLOSING;
    if (is_syn(s->tcp_flags))
        return FS_HALF_OPEN;
    return FS_ESTABLISHED;
}


/* The harness does not check init's result: it goes on to call the copy's hash with a NULL lb and crashes
   (segfault in hash). So a setup error must end the run here, with a message, rather than return NULL. */
/* Responsible: Atieh */
static void init_fail(const char *msg)
{
    fprintf(stderr, "init: %s\n", msg);
    exit(1);
}

/* A whole-number parameter in [min, max], def if absent; a bad value ends the run (rejects "", "abc", "100x"). */
/* Responsible: Ludovic */
static unsigned long param_ul(gen_t *g, const char *key, unsigned long def, unsigned long min, unsigned long max)
{
    const char *v = gen_param(g, key);
    if (!v)
        return def;
    char *end = NULL;
    unsigned long x = strtoul(v, &end, 10);
    if (end == v || *end != '\0' || x < min || x > max) {
        fprintf(stderr, "init: %s=%s: must be a whole number between %lu and %lu\n", key, v, min, max);
        exit(1);
    }
    return x;
}

#define MS 1000000ull           /* ns per ms */
#define TO_MAX_MS 86400000ul    /* timeouts: at most a day */

/* Timeouts, in ms: to_half_ms, to_est_ms, to_fin_ms for every service, then optional overrides per service,
   keyed by its VIP */
/* Responsible: Ludovic */
static void init_timeouts(gen_t *g, lb_t *lb)
{
    static const char *const names[3] = { "to_half_ms", "to_est_ms", "to_fin_ms" };
    static const unsigned long defaults[3] = { 1000, 130000, 2000 };
    unsigned long base[3];
    for (int k = 0; k < 3; k++)
        base[k] = param_ul(g, names[k], defaults[k], 1, TO_MAX_MS);

    uint32_t n = lb->cfg.n_services;
    lb->to = gen_alloc(g, (n ? n : 1) * sizeof *lb->to, 0, 0);
    if (!lb->to)
        init_fail("gen_alloc for the timeouts failed");
    for (uint32_t i = 0; i < n; i++) {
        uint32_t vip = lb->cfg.services[i].vip;
        unsigned long ms[3];
        for (int k = 0; k < 3; k++) {
            char key[64];
            snprintf(key, sizeof key, "%s_%u.%u.%u.%u", names[k],
                     vip >> 24, (vip >> 16) & 0xff, (vip >> 8) & 0xff, vip & 0xff);
            ms[k] = param_ul(g, key, base[k], 1, TO_MAX_MS);
        }
        lb->to[i].half = ms[0] * MS;
        lb->to[i].est = ms[1] * MS;
        lb->to[i].fin = ms[2] * MS;
    }
}

/* Responsible: Ludovic (parameters, setup), Samuel (flow table), Atieh (rings, prefetch and E4 parameters) */
static lb_t *init(gen_t *g, const gen_config_t *cfg)
{
    lb_t *lb = gen_alloc(g, sizeof(lb_t), 0, 0);
    if (!lb)
        init_fail("gen_alloc for LB failed");
    memset(lb, 0, sizeof *lb);
    lb->cfg = *cfg;

    /* Replica selection (Atieh): one ring per service, ring_v points per replica (default 100), or, for one
       service, ring_v_<its VIP> (e.g. ring_v_198.51.100.10=1000). E3 picks v per service: more points balance
       better but make the ring bigger and each lookup and replica event slower, and the services differ in how
       much they gain and how often they look the ring up (web: every new flow and every SYN of a flood). */
    uint32_t v = (uint32_t)param_ul(g, "ring_v", 100, 1, 100000);
    lb->rings = gen_alloc(g, (cfg->n_services ? cfg->n_services : 1) * sizeof *lb->rings, 0, 0);
    if (!lb->rings)
        init_fail("gen_alloc for rings failed");
    for (uint32_t k = 0; k < cfg->n_services; k++) {
        uint32_t vip = cfg->services[k].vip;
        char key[48];
        snprintf(key, sizeof key, "ring_v_%u.%u.%u.%u", vip >> 24, (vip >> 16) & 0xff, (vip >> 8) & 0xff,
                 vip & 0xff);
        uint32_t vk = (uint32_t)param_ul(g, key, v, 1, 100000);
        if (!ring_init(&lb->rings[k], g, &cfg->services[k], vk, cfg->seed))
            init_fail("gen_alloc for a service's ring failed (ring_v too large for the memory budget?)");
    }
    lb->ev_coalesce = (uint8_t)param_ul(g, "ev_coalesce", 0, 0, 1);
    for (uint32_t k = 0; lb->ev_coalesce && k < cfg->n_services; k++)
        if (!ring_enable_coalescing(&lb->rings[k]))
            init_fail("gen_alloc for event coalescing failed");

    const char *hf = gen_param(g, "hashfn");
    if (!hf || strcmp(hf, "tab") == 0) {
        lb->hashfn = HASH_TAB;
    } else if (strcmp(hf, "crc") == 0) {
        lb->hashfn = HASH_CRC;
    } else {
        init_fail("bad hashfn value: tab or crc");
    }

    lb->th = gen_alloc(g, sizeof(tabhash_t), 64, 0);
    if (!lb->th) {
        init_fail("gen_alloc for the tabulation tables failed");
    }
    tabhash_init(lb->th, cfg->seed);

    uint32_t nb = (uint32_t)param_ul(g, "ft_buckets", 65536, 2, UINT32_MAX);
    uint32_t bs = (uint32_t)param_ul(g, "bucket_size", 10, 2, 64);
    uint32_t md = (uint32_t)param_ul(g, "max_depth", 5, 2, 16);
    uint32_t dfs = (uint32_t)param_ul(g, "dfs_budget", 64, 2, 100000);
    // signature width: 0 = every bit the entry index leaves free
    uint8_t sb = (uint8_t)param_ul(g, "sig_bits", 0, 0, FT_MAX_SIG_BITS);
    uint8_t ip = (uint8_t)param_ul(g, "insert_policy", 0, 0, 2);

    init_timeouts(g, lb);

    lb->ft = ft_init(g, nb, bs, md, dfs, sb,
                    lb->hashfn, lb->th, cfg->seed, lb->to);

    if (!lb->ft) {
        init_fail("flow table initialization failed");
    }
    lb->ft->insert_policy = ip;

    /* Prefetching (E1, Atieh): off unless the params turn it on, so that it can be measured on and off */
    lb->pf_pkt = (uint8_t)param_ul(g, "pf_pkt", 0, 0, 1);
    lb->pf_bucket = (uint8_t)param_ul(g, "pf_bucket", 0, 0, 2);
    lb->pf_entry = (uint8_t)param_ul(g, "pf_entry", 0, 0, 2);
    lb->pf_dist = (uint32_t)param_ul(g, "pf_dist", 0, 0, 64);
    lb->batch = (uint8_t)param_ul(g, "batch", 64, 1, 64);
    uint8_t fb = (uint8_t)param_ul(g, "ins_fallback", 0, 0, 3);
    for (uint32_t k = 0; k < 16; k++) {
        lb->fallback[k] = fb;
        if (k < cfg->n_services) {
            uint32_t vip = cfg->services[k].vip;
            char key[48];
            snprintf(key, sizeof key, "ins_fallback_%u.%u.%u.%u", vip >> 24, (vip >> 16) & 0xff, (vip >> 8) & 0xff,
                     vip & 0xff);
            lb->fallback[k] = (uint8_t)param_ul(g, key, fb, 0, 3);
        }
    }

    /* E5 (Ludovic): SYN-flood defence and occupancy probe, see struct lb */
    lb->syn_insert = (uint8_t)param_ul(g, "syn_insert", 1, 0, 1);
    unsigned long guard = param_ul(g, "syn_guard", 0, 0, 100000000);
    lb->syn_guard_win = (uint32_t)((guard + 9) / 10);
    if (lb->syn_guard_win) {
        uint32_t n = cfg->n_services ? cfg->n_services : 1;
        lb->guard_start = gen_alloc(g, n * sizeof *lb->guard_start, 0, 0);
        lb->guard_count = gen_alloc(g, n * sizeof *lb->guard_count, 0, 0);
        if (!lb->guard_start || !lb->guard_count)
            init_fail("gen_alloc for the SYN guard failed");
        memset(lb->guard_start, 0, n * sizeof *lb->guard_start);
        memset(lb->guard_count, 0, n * sizeof *lb->guard_count);
    }
    lb->occ_every_ns = param_ul(g, "occ_every_s", 0, 0, 86400) * 1000000000ull;
    lb->occ = NULL;
    return lb;
}

/* Responsible: Atieh. Replica events update the service's ring (incrementally, see src/ring.c). */
static void on_event(lb_t *lb, const gen_event_t *ev)
{
    if (ev->service >= lb->cfg.n_services)
        return;
    /* ring_init reserved room for the published scenarios' churn, so this does not allocate there. If it
       has to and gen_alloc fails, this instance's ring no longer matches the copy's: say so loudly. */
    ring_t *r = &lb->rings[ev->service];
    if (!(lb->ev_coalesce ? ring_event_defer(r, (int)ev->type, ev->ipv4) : ring_event(r, (int)ev->type, ev->ipv4)))
        fprintf(stderr, "ring: gen_alloc failed on a replica event (service %u, type %d): ring not updated\n",
                ev->service, (int)ev->type);
}

/* Responsible: Ludovic, then Samuel (parser, hash); Atieh (ft_locate once per packet, ev_coalesce, prefetching) */

static void hash(lb_t *lb, pkt_t **p, uint8_t n,
                 pkt_state_t *s, uint64_t mask)
{
    /* ev_coalesce (E4): the replica events recorded since the last batch move the ring's points now, together,
       before lookup and compute_replica use them (the harness's copy calls hash first too) */
    if (lb->ev_coalesce)
        for (uint32_t k = 0; k < lb->cfg.n_services; k++)
            ring_flush(&lb->rings[k]);
    /* pf_pkt (Atieh): the packet buffers live in a pool far larger than the cache, so reading each packet's headers
       is a cache miss. Ask for them ahead (all of them first with pf_dist=0, else pf_dist packets ahead); the loop
       then parses packets whose headers are already on their way. */
    uint32_t d = lb->pf_dist;
    if (lb->pf_pkt) {
        uint32_t first = d == 0 || d > n ? n : d;
        for (uint32_t i = 0; i < first; i++)
            if (mask & (1ull << i))
                prefetch_headers(p[i]);
    }

    for (uint8_t i = 0; i < n; i++) {
        if (lb->pf_pkt && d && i + d < n && (mask & (1ull << (i + d))))
            prefetch_headers(p[i + d]);
        if (!(mask & (1ull << i)))
            continue;

        // Initialize packet state
        memset(&s[i], 0, sizeof(s[i]));
        const uint8_t *f = p[i]->buf + p[i]->off;
        uint16_t len = p[i]->len;

        // Parse Ethernet, IPv4 and TCP/UDP
        if (!parse_flow(f, len, &s[i].key, &s[i].tcp_flags))
            continue;
        flow_key_t *key = &s[i].key;

        // Find the corresponding service
        for (uint32_t y = 0; y < lb->cfg.n_services; y++) {

            service_t service = lb->cfg.services[y];

            if (service.proto == key->protocol && service.vip == key->dst_ip && service.port == key->dst_port) {
                s[i].service_id = y;
                s[i].valid = 1;
                break;
            }
        }

        if (!s[i].valid)
            continue;
        // Hash the 5-tuple: simple tabulation (default) or CRC32C (baseline)
        s[i].hash = lb->hashfn == HASH_TAB ? tabhash_flow(lb->th, key) : crc_flow(lb->cfg.seed, key);

        /* Where the key can be in the flow table, once for lookup, insert and the prefetches (Atieh) */
        ft_locate(lb->ft, s[i].hash, &s[i].loc);

        /* pf_bucket with pf_dist=0 (Atieh): the two buckets lookup will read for this packet, asked for now so that
           they arrive while hash works on the rest of the batch (with pf_dist > 0, lookup asks for them) */
        if (lb->pf_bucket && d == 0)
            ft_prefetch_buckets(lb->ft, &s[i].loc, lb->pf_bucket == 2);
    }
}


/* (Atieh) What the flow table needs from replica selection -- measured with a stand-in table:
 *   - store the replica as its id, ring_replica_id(&lb->rings[svc], s[i].replica) at insert (16 bits are
 *     enough), and read it back with ring_ip(); ids never change during a run;
 *   - on a hit, if ring_state(r, id) == REPLICA_STATE_DOWN: treat it as a miss (the flow is lost; its
 *     packets must go elsewhere, and its client's retransmitted SYN must not go back to the dead replica);
 *   - on a hit by a SYN (SYN set, ACK not set), if ring_state(r, id) != REPLICA_STATE_ACTIVE: treat it as a
 *     miss too. A retransmitted SYN is a new-flow decision: without this, a SYN whose replica failed
 *     silently and was then drained is sent to a draining replica (44 misplaced flows on events.scn).
 *   With neither check, tests/failover.scn and tests/churn.scn fail (52 and 83 misplaced flows). */
/* Responsible: Ludovic (stage), Samuel (flow-table calls), Atieh (ft_loc, prefetching, down-replica and SYN rules) */
static uint64_t lookup(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask)
{
    /* Prefetching (Atieh). pf_dist=0: pf_entry reads every packet's buckets first (in cache already with pf_bucket,
       which hash asked for) and asks for the entries they point to with its signature; the loop then finds them
       arriving or arrived. pf_dist=D: while it looks packet i up, it asks for the buckets of packet i + D (of
       i + 2D with pf_entry, so that they have arrived when the entries of that packet are asked for, D packets
       later) and for the entries of packet i + D. */
    uint32_t d = lb->pf_dist;
    uint32_t bd = lb->pf_entry ? 2 * d : d;
#define PF_OK(j) ((j) < n && (mask & (1ull << (j))) && s[(j)].valid)
    if (d == 0) {
        if (lb->pf_entry)
            for (uint32_t i = 0; i < n; i++)
                if (PF_OK(i))
                    ft_prefetch_entries(lb->ft, &s[i].loc, lb->pf_entry == 2);
    } else if (lb->pf_bucket) {
        for (uint32_t i = 0; i < bd && i < n; i++)
            if (PF_OK(i))
                ft_prefetch_buckets(lb->ft, &s[i].loc, lb->pf_bucket == 2);
    }

    uint64_t hit = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (d) {
            if (lb->pf_bucket && PF_OK(i + bd))
                ft_prefetch_buckets(lb->ft, &s[i + bd].loc, lb->pf_bucket == 2);
            if (lb->pf_entry && PF_OK(i + d))
                ft_prefetch_entries(lb->ft, &s[i + d].loc, lb->pf_entry == 2);
        }
        if (!(mask & (1ull << i)) || !s[i].valid)
            continue;
        uint64_t now = p[i]->ts_ns;
        flow_entry_t *e = ft_find_at(lb->ft, &s[i].key, &s[i].loc, now);
        if (!e)
            continue;                                       /* miss: not in the table, or expired */

        const ring_t *r = &lb->rings[s[i].service_id];
        uint8_t f = s[i].tcp_flags;
        int syn = s[i].key.protocol == 6 && is_syn(f);
        uint8_t rs = ring_state(r, e->replica);
        /* Atieh's two rules: a down replica, or a SYN (a new-flow decision) to a replica that is not active,
           is a miss. The entry stays: insert overwrites it with compute_replica's choice. */
        if (rs == REPLICA_STATE_DOWN || (syn && rs != REPLICA_STATE_ACTIVE))
            continue;
        s[i].replica = ring_ip(r, e->replica);

        /* Lifecycle (brief, item 5): refresh the last-seen time; FIN or RST starts the closing grace period,
           which later packets do not extend. */
        /* last_seen is refreshed by every packet except those of a closing flow: there it stays the time of the
           FIN or RST, which starts the grace period (is_expired, src/flow_table.c) */
        if (s[i].key.protocol == 6 && (f & (TCP_FIN | TCP_RST))) {
            if (e->state != FS_CLOSING) {
                e->state = FS_CLOSING;
                if (now > e->last_seen)
                    e->last_seen = now;
            }
        } else if (syn) {
            e->state = FS_HALF_OPEN;
            if (now > e->last_seen)
                e->last_seen = now;
        } else if (e->state != FS_CLOSING) {
            if (e->state == FS_HALF_OPEN &&
                (s[i].key.protocol != 6 || (f & TCP_ACK))) {
                e->state = FS_ESTABLISHED;
            }
            if (now > e->last_seen)
                e->last_seen = now;
        }
        hit |= 1ull << i;
    }
#undef PF_OK
    return hit;
}

/* Responsible: Atieh. Consistent hashing over the service's active replicas.
   Works on its own (the harness's copy calls it right after hash, without lookup). */
static uint64_t compute_replica(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask)
{
    (void)p;
    uint64_t ok = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (!(mask & (1ull << i)) || !s[i].valid)
            continue;
        uint32_t ip = ring_lookup(&lb->rings[s[i].service_id], s[i].hash);
        if (!ip)
            continue;   /* no active replica in this service */
        s[i].replica = ip;
        ok |= 1ull << i;
    }
    return ok;
}

/* Stores the flows compute_replica gave a replica: new flows (SYN), flows first seen mid-flow (from another LB, or
   evicted earlier), and flows whose stored replica lookup rejected. ft_insert is an upsert, so two packets of one
   new flow in the same batch (same hash, same ring: same replica) store a single entry. A failed insert only
   clears the packet's bit: the main loop forwards it anyway, by consistent hashing (brief, item 5). */
/* Responsible: Ludovic (stage), Samuel (flow-table calls), Atieh (ins_fallback, E4) */
static uint64_t insert(lb_t *lb, pkt_t **p, uint8_t n, pkt_state_t *s, uint64_t mask)
{
    uint64_t stored = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (!(mask & (1ull << i)) || !s[i].valid)
            continue;
        uint32_t svc = s[i].service_id;
        uint32_t id = ring_replica_id(&lb->rings[svc], s[i].replica);
        if (id == RING_NO_ID)
            continue;                       /* cannot happen: compute_replica returns a replica of the ring */
        uint64_t now = p[i]->ts_ns;
        uint8_t st = first_state(&s[i]);

        if (ft_insert_at(lb->ft, &s[i].key, id, st, &s[i].loc,
                    svc, now))
            stored |= 1ull << i;
        else if (svc < 16 && lb->fallback[svc] &&
                 ft_evict_insert_at(lb->ft, &s[i].key, id, st, &s[i].loc, svc, now, lb->fallback[svc])) {
            stored |= 1ull << i;
#if E4_STATS
            if (svc < 16)
                lb->e4_evict[svc]++;
#endif
        }
    }
    return stored;
}

/* Responsible: Ludovic */
static uint32_t replica_of(const pkt_state_t *s)
{
    return s->replica;
}

/* IP-in-IP encapsulation, ported from Atieh's test rig (scripts/tests_ring/e2e_main.c):
   the new Ethernet and outer IPv4 headers go over the 20 bytes of headroom and the old Ethernet header; the inner
   packet does not move. Responsible: Ludovic (from Atieh's test rig) */
static uint16_t ip_checksum(const uint8_t *h)
{
    uint32_t sum = 0;
    for (int i = 0; i < 20; i += 2)
        sum += ((uint32_t)h[i] << 8) | h[i + 1];
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static void put32(uint8_t *b, uint32_t x)
{
    b[0] = (uint8_t)(x >> 24); b[1] = (uint8_t)(x >> 16); b[2] = (uint8_t)(x >> 8); b[3] = (uint8_t)x;
}

static void encap(const lb_t *lb, pkt_t *pk, uint32_t dst)
{
    uint8_t *f = pk->buf + pk->off - 20;                        /* the new frame starts in the headroom */
    uint16_t inner_len = rd16(f + 20 + 14 + 2);                 /* the inner IPv4 total length */
    memcpy(f, lb->cfg.zone_mac, 6);
    memcpy(f + 6, lb->cfg.int_mac, 6);
    f[12] = 0x08; f[13] = 0x00;
    uint8_t *ip = f + 14;
    uint16_t tot = (uint16_t)(inner_len + 20);
    ip[0] = 0x45; ip[1] = 0;
    ip[2] = (uint8_t)(tot >> 8); ip[3] = (uint8_t)tot;
    ip[4] = ip[5] = ip[6] = ip[7] = 0;                          /* ID, flags, fragment offset */
    ip[8] = 64; ip[9] = 4;                                      /* TTL, protocol IP-in-IP */
    ip[10] = ip[11] = 0;
    put32(ip + 12, lb->cfg.int_ipv4);
    put32(ip + 16, dst);
    uint16_t c = ip_checksum(ip);
    ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)c;
    pk->off = (uint16_t)(pk->off - 20);
    pk->len = (uint16_t)(pk->len + 20);
}

static const lb_ops_t ops = {
    .state_size = sizeof(pkt_state_t),
    .init = init,
    .on_event = on_event,
    .hash = hash,
    .lookup = lookup,
    .compute_replica = compute_replica,
    .insert = insert,
    .replica_of = replica_of,
};

/* ---- E3 (Atieh): flows and packets per replica, for the balance measurements of scripts/e3/run_e3.sh ----
   Off unless the LB is compiled with -DE3_STATS=1 (run_e3.sh builds that copy as build/lb_e3); the normal build has
   none of it. Counted on the main LB's loop only, just before each batch is sent: a packet sent counts for its
   replica, a packet that needed compute_replica (a flow-table miss: in a run without churn, a new flow) counts one
   flow for it. Every second of trace time, the most loaded replica's packets over the average gives the imbalance
   of that second, the load a replica actually sees at a given moment. */
#ifndef E3_STATS
#define E3_STATS 0
#endif
#if E3_STATS
#define E3_SVC 16
#define E3_REP 512
#define E3_SEC 4096
typedef struct {
    uint64_t flows[E3_SVC][E3_REP], pkts[E3_SVC][E3_REP], sec[E3_SVC][E3_REP];
    float ratio[E3_SVC][E3_SEC];   /* per second: most loaded active replica's packets / average */
    uint32_t n_sec[E3_SVC];
    uint64_t sec_end;              /* end of the current second of trace time */
} e3_t;

/* Responsible: Atieh (E3 statistics, this function and the two below) */
static void e3_close_second(e3_t *e, const lb_t *lb)
{
    for (uint32_t sv = 0; sv < lb->cfg.n_services && sv < E3_SVC; sv++) {
        const ring_t *r = &lb->rings[sv];
        uint64_t sum = 0, mx = 0;
        uint32_t act = 0;
        for (uint32_t id = 0; id < r->n_reps && id < E3_REP; id++) {
            if (ring_state(r, id) == REPLICA_STATE_ACTIVE) {
                act++;
                sum += e->sec[sv][id];
                if (e->sec[sv][id] > mx)
                    mx = e->sec[sv][id];
            }
            e->sec[sv][id] = 0;
        }
        if (act && sum && e->n_sec[sv] < E3_SEC)
            e->ratio[sv][e->n_sec[sv]++] = (float)((double)mx * act / (double)sum);
    }
}

static int e3_cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static void e3_report(e3_t *e, const lb_t *lb)
{
    for (uint32_t sv = 0; sv < lb->cfg.n_services && sv < E3_SVC; sv++) {
        const ring_t *r = &lb->rings[sv];
        uint64_t fs = 0, fm = 0, ps = 0, pm = 0;
        uint32_t act = 0;
        for (uint32_t id = 0; id < r->n_reps && id < E3_REP; id++) {
            if (ring_state(r, id) != REPLICA_STATE_ACTIVE)
                continue;
            act++;
            fs += e->flows[sv][id];
            ps += e->pkts[sv][id];
            if (e->flows[sv][id] > fm) fm = e->flows[sv][id];
            if (e->pkts[sv][id] > pm) pm = e->pkts[sv][id];
            uint32_t ip = ring_ip(r, id);
            fprintf(stderr, "E3R svc=%u ip=%u.%u.%u.%u flows=%llu pkts=%llu\n", sv, ip >> 24, ip >> 16 & 255,
                    ip >> 8 & 255, ip & 255, (unsigned long long)e->flows[sv][id], (unsigned long long)e->pkts[sv][id]);
        }
        uint32_t ns = e->n_sec[sv];
        qsort(e->ratio[sv], ns, sizeof(float), e3_cmp_float);
        fprintf(stderr, "E3 svc=%u v=%u secret_seed=%016llx active=%u flows=%llu pkts=%llu flows_maxavg=%.4f "
                "pkts_maxavg=%.4f sec=%u sec_median=%.4f sec_p95=%.4f\n", sv, r->v, (unsigned long long)lb->cfg.seed,
                act, (unsigned long long)fs,
                (unsigned long long)ps, fs && act ? (double)fm * act / (double)fs : 0,
                ps && act ? (double)pm * act / (double)ps : 0, ns, ns ? e->ratio[sv][ns / 2] : 0,
                ns ? e->ratio[sv][(ns * 95) / 100] : 0);
    }
}
#endif

#if E4_STATS
/* Per service and per second of trace time: valid packets, misses (replica computed), new flows (SYN among the
   misses), failed inserts (after the fallback), evictions; and, at the end of each second, the service's entries by
   state, and how many of them have expired (their flow is over: still in the table until an insert reuses the slot). */
typedef struct {
    uint64_t rx[16], miss[16], syn[16], fail[16], evict_seen[16];
    uint64_t sec_end;
} e4_t;

/* Responsible: Atieh (E4 statistics, this function and the one below) */
static void e4_close_second(e4_t *e, lb_t *lb, uint64_t now)
{
    uint32_t half[16] = {0}, est[16] = {0}, fin[16] = {0}, expired[16] = {0};
    const flow_table_t *ft = lb->ft;
    for (uint32_t i = 0; i < ft->capacity; i++) {
        uint32_t sl = ft->slots[i];
        if (!sl)
            continue;
        const flow_entry_t *en = &ft->entries[(sl >> ft->sig_bits) - 1];
        uint32_t sv = en->service_id < 16 ? en->service_id : 15;
        const svc_timeouts_t *to = &lb->to[en->service_id];
        uint64_t limit = en->state == FS_HALF_OPEN ? to->half : en->state == FS_CLOSING ? to->fin : to->est;
        if (now >= en->last_seen && now - en->last_seen >= limit)
            expired[sv]++;                          /* as is_expired (src/flow_table.c) decides */
        else if (en->state == FS_HALF_OPEN)
            half[sv]++;
        else if (en->state == FS_CLOSING)
            fin[sv]++;
        else
            est[sv]++;
    }
    for (uint32_t sv = 0; sv < lb->cfg.n_services && sv < 16; sv++) {
        uint64_t ev = lb->e4_evict[sv] - e->evict_seen[sv];
        e->evict_seen[sv] = lb->e4_evict[sv];
        fprintf(stderr, "E4T t=%llu svc=%u half_open=%u established=%u closing=%u expired=%u rx=%llu miss=%llu "
                "syn=%llu ins_fail=%llu evict=%llu\n", (unsigned long long)(e->sec_end / 1000000000ull), sv,
                half[sv], est[sv], fin[sv], expired[sv], (unsigned long long)e->rx[sv],
                (unsigned long long)e->miss[sv], (unsigned long long)e->syn[sv], (unsigned long long)e->fail[sv],
                (unsigned long long)ev);
        e->rx[sv] = e->miss[sv] = e->syn[sv] = e->fail[sv] = 0;
    }
}

static void e4_count(e4_t *e, lb_t *lb, pkt_t **p, uint8_t n, const pkt_state_t *s, uint64_t computed,
                     uint64_t got, uint64_t stored)
{
    for (uint8_t i = 0; i < n; i++) {
        if (!s[i].valid)
            continue;
        while (p[i]->ts_ns >= e->sec_end) {
            e4_close_second(e, lb, e->sec_end);
            e->sec_end += 1000000000ull;
        }
        uint32_t sv = s[i].service_id < 16 ? s[i].service_id : 15;
        uint64_t bit = 1ull << i;
        e->rx[sv]++;
        if (computed & bit) {
            e->miss[sv]++;
            if (s[i].key.protocol == 6 && is_syn(s[i].tcp_flags))
                e->syn[sv]++;
        }
        if ((got & bit) && !(stored & bit))
            e->fail[sv]++;
    }
}
#endif

/* Responsible: Ludovic (loop), Samuel (flow table), Atieh (batch, E3/E4 hooks) */
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s SCENARIO [PARAMS]\n", argv[0]);
        return 2;
    }
    gen_t *g = gen_open(argv[1], argc > 2 ? argv[2] : "", &ops);
    if (!g) return 1;
    lb_t *lb =gen_lb(g);
    pkt_t *p[64];
    pkt_state_t s[64];
#if E3_STATS
    static e3_t e3;
    e3.sec_end = 1000000000ull;
#endif
#if E4_STATS
    static e4_t e4;
    e4.sec_end = 1000000000ull;
#endif

    /* E5 occupancy probe: opened here, for the main instance only (init also runs for the harness's copy) */
    uint32_t nsvc = lb->cfg.n_services;
    uint64_t occ_live[FT_OCC_MAX_SVC * 3];
    if (lb->occ_every_ns) {
        const char *path = gen_param(g, "occ_file");
        lb->occ = fopen(path ? path : "occupancy.csv", "w");
        if (!lb->occ || nsvc > FT_OCC_MAX_SVC) {
            fprintf(stderr, "occ: cannot open the occupancy file, or too many services\n");
            return 1;
        }
        fprintf(lb->occ, "t_s,service,half_open,established,closing,expired,capacity\n");
        lb->occ_next_ns = lb->occ_every_ns;
    }

    while (gen_running(g)) {
        /* Tunnelled replies (bonus, web_tunnel=1) are not handled: drop whatever reaches the internal NIC. */
        uint8_t m = gen_rx_batch(g, GEN_NIC_INT, p, 64);
        if (m)
            gen_drop(g, p, m, ~0ull);

        uint8_t n = gen_rx_batch(g, GEN_NIC_EXT, p, lb->batch);
        if (n == 0)
            continue;
        uint64_t all = n == 64 ? ~0ull : (1ull << n) - 1;
        gen_hash(g, lb, p, n, s, all);
        uint64_t ok = 0;                                        /* the packets of a service */
        for (uint8_t i = 0; i < n; i++) {
            if (s[i].valid) {
                ok |= (1ull << i);
            }
        }
        uint64_t hit = gen_lookup(g, lb, p, n, s, ok);
        uint64_t got = gen_replica(g, lb, p, n, s, ok & ~hit);
        uint64_t to_store = got;
        if (!lb->syn_insert || lb->syn_guard_win) {             /* E5 defence: SYNs forwarded, not stored */
            for (uint8_t i = 0; i < n; i++) {
                if (!(got & (1ull << i)) || s[i].key.protocol != 6 || !is_syn(s[i].tcp_flags))
                    continue;
                if (lb->syn_insert) {                           /* syn_guard: within the service's quota? */
                    uint32_t k = s[i].service_id;
                    uint64_t now = p[i]->ts_ns;
                    if (now - lb->guard_start[k] >= 100 * MS) {
                        lb->guard_start[k] = now;
                        lb->guard_count[k] = 0;
                    }
                    if (lb->guard_count[k] < lb->syn_guard_win) {
                        lb->guard_count[k]++;
                        continue;                               /* stored */
                    }
                }
                to_store &= ~(1ull << i);
            }
        }
#if E4_STATS
        uint64_t stored = gen_insert(g, lb, p, n, s, to_store); /* sent even if the insert fails */
        e4_count(&e4, lb, p, n, s, ok & ~hit, got, stored);
#else
        gen_insert(g, lb, p, n, s, to_store);                   /* sent even if the insert fails */
#endif
        uint64_t send = hit | got;
#if E3_STATS
        for (uint8_t i = 0; i < n; i++) {
            if (!(send & (1ull << i)))
                continue;
            while (p[i]->ts_ns >= e3.sec_end) {
                e3_close_second(&e3, lb);
                e3.sec_end += 1000000000ull;
            }
            uint32_t sv = s[i].service_id, id = ring_replica_id(&lb->rings[sv], s[i].replica);
            if (sv < E3_SVC && id < E3_REP) {
                e3.pkts[sv][id]++;
                e3.sec[sv][id]++;
                if (got & (1ull << i))
                    e3.flows[sv][id]++;
            }
        }
#endif
        for (uint8_t i = 0; i < n; i++)
            if (send & (1ull << i))
                encap(lb, p[i], s[i].replica);
        gen_tx_batch(g, GEN_NIC_INT, p, n, send);
        gen_drop(g, p, n, all & ~send);

        if (lb->occ) {
            uint64_t now = p[n - 1]->ts_ns;
            while (now >= lb->occ_next_ns) {
                uint64_t expired = 0;
                ft_occupancy(lb->ft, now, nsvc, occ_live, &expired);
                for (uint32_t k = 0; k < nsvc; k++)
                    fprintf(lb->occ, "%.0f,%u,%llu,%llu,%llu,%llu,%u\n", lb->occ_next_ns / 1e9, k,
                            (unsigned long long)occ_live[3 * k], (unsigned long long)occ_live[3 * k + 1],
                            (unsigned long long)occ_live[3 * k + 2], (unsigned long long)(k == 0 ? expired : 0),
                            lb->ft->capacity);
                lb->occ_next_ns += lb->occ_every_ns;
            }
        }
    }
    if (lb->occ)
        fclose(lb->occ);
#if FT_STATS
    /* E1: the main LB's flow-table counters, raw, on one line of stderr (key=value), with the table's layout and
       its occupancy at the end. scripts/tests_ft/false_matches.sh parses it and compares with the theory. */
    const flow_table_t *ft = lb->ft;
    const ft_stats_t *st = &ft->stats;
    uint64_t used = 0;
    for (uint32_t i = 0; i < ft->capacity; i++)
        used += ft->slots[i] != 0;
    fprintf(stderr, "ft_stats: sig_bits=%u num_buckets=%u bucket_size=%u capacity=%u slots_used=%llu lookups=%llu "
            "hits=%llu slots_checked=%llu sig_matches=%llu false_matches=%llu\n",
            ft->sig_bits, ft->num_buckets, ft->bucket_size, ft->capacity, (unsigned long long)used,
            (unsigned long long)st->lookups, (unsigned long long)st->hits, (unsigned long long)st->slots_checked,
            (unsigned long long)st->sig_matches, (unsigned long long)st->false_matches);

    fprintf(stderr, "\n--- E1 statistics ---\n");

    fprintf(stderr, "Occupied slots: %u / %u\n",
            st->occupied_slots, ft->capacity);

    if (st->first_failure_seen)
    {
        double load_factor =
            100.0 * st->first_failure_occupied / ft->capacity;

        fprintf(stderr, "First failure occupied: %u\n",
                st->first_failure_occupied);

        fprintf(stderr, "First failure load factor: %.2f%%\n",
                load_factor);
    }
    else
    {
        fprintf(stderr, "No insertion failure\n");
    }

    fprintf(stderr, "\nLoad range,Insertions,Displacements,Average\n");

    for (int i = 0; i < 10; i++)
    {
        double average = 0.0;

        if (st->insertions_by_load[i] > 0)
        {
            average = (double)st->displacements_by_load[i]
                    / st->insertions_by_load[i];
        }

        fprintf(stderr, "%d-%d%%,%llu,%llu,%.6f\n",
                i * 10,
                (i + 1) * 10,
                (unsigned long long)st->insertions_by_load[i],
                (unsigned long long)st->displacements_by_load[i],
                average);
    }
#endif
#if E3_STATS
    e3_report(&e3, lb);
#endif
    gen_close(g);
    return 0;
}

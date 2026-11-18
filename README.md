# INFO8011: an L4 load balancer, and the harness that tests it

You write, in C, the data plane of an L4 load balancer (LB). The **harness** (`gen/libgen.a`, its API in
`gen/gen.h`) plays your LB's two network cards, generates the traffic, checks every forwarding decision and
measures everything, so that every LB is tested and measured by the same code. This document explains what the
harness does and what it expects from you.

Contents: [the skeleton](#the-skeleton) · [building and running](#building-and-running) ·
[the setting](#the-setting) · [your LB](#your-lb) · [packets](#packets) · [replica events](#replica-events) ·
[the second copy](#the-second-copy-and-the-other-lb-instances) · [memory](#memory) · [parameters](#parameters) ·
[modes](#modes) · [outcomes](#outcomes) · [the results file](#the-results-file) ·
[testing and debugging](#testing-and-debugging) · [bonus](#bonus-web-in-a-two-way-tunnel) · [CRC32C](#crc32c)

## The skeleton

```
Makefile          provided: do not modify it (the submission platform uses its own copy)
gen/              the harness: gen.h, crc32c.h, libgen.a for x86_64 and aarch64
src/              your components: any .c and .h, subfolders allowed
lb/main.c         your LB's main program (a stub that drops every packet)
autotune/main.c   chooses parameters for your LB on the machine it runs on (a stub that chooses none)
config.params     your fixed parameters, key=value,... on one line (empty)
scripts/          your scripts (experiments, plots)
scenarios/        the published scenarios (*.scn) and their format, FORMAT.md
tests/            the test suite
```

The published scenarios: `chat.scn`, `video.scn` and `web.scn` (each service alone, with replica churn),
`mixed.scn` (the three, with replica churn and LB churn), `mixed_flood.scn` (the contest scenario: `mixed` plus a
SYN flood on web), and `events.scn` (one service with 500 replicas and many replica events, to measure the cost of
your `on_event`). In functional mode, with a simple LB on a laptop, each takes from a few seconds to about a
minute.

## Building and running

Linux on x86-64 or aarch64 (an aarch64 Linux VM on a Mac is fine), GCC or Clang.

| Command | What it does |
|---|---|
| `make` | builds `build/lb` and `build/autotune` |
| `make debug` | the same in `build-debug/`, with AddressSanitizer and UndefinedBehaviorSanitizer |
| `make test` | builds, then runs the test suite on `build/` (`tests/run.sh build-debug` runs it on the debug build) |
| `make clean` | removes `build/` and `build-debug/` |

Every `.c` file under `src/` is compiled into `build/libcomp.a`; `lb/main.c` and `autotune/main.c` are each linked
with it and with `libgen.a`. Put your components in `src/`, so that both programs can use them. The flags are fixed:
`-std=c11 -O2 -Wall -Wextra`, plus the CRC instructions of the architecture, include paths `src/` and `gen/`.
Nothing is taken from the environment except `CC` (e.g. `make CC=clang`). With `-std=c11`, POSIX functions are not
declared unless you ask for them: put `#define _POSIX_C_SOURCE 200809L` (or `_GNU_SOURCE`) before the first
`#include` of a file that needs them.

**Command-line contract.** The test suite and the submission platform run your programs like this:

- `build/lb SCENARIO "PARAMS"` runs your LB on a scenario with a parameter string (`key=value,...`), then exits.
  The harness writes the results file (path from the `results` key, default `results.csv` in the current
  directory).
- `build/autotune` takes no argument and prints **exactly one line** of `key=value` pairs separated by commas on
  standard output (an empty line if it has nothing to say), and nothing else: diagnostics go to standard error. It
  may measure your components with `gen_open` in bench mode (see [modes](#modes)).
- The contest runs: `build/lb mixed_flood.scn "$(build/autotune),$(cat config.params),KEYS"` (here:
  `build/lb scenarios/mixed_flood.scn ...`), where `KEYS` are the [harness keys](#parameters) the platform sets for
  that run (`mode`, `results`, ...). The test suite builds its parameter strings the same way: your autotuner's
  line, then `config.params`, then the keys of the run. A key may appear only once in the whole string, so do not
  set a harness key that the run sets itself (`results` always): `gen_open` would fail.

So `lb/main.c` must take the scenario as `argv[1]` and the parameters as `argv[2]`, and pass both to `gen_open`.

## The setting

- Your LB is **one instance of an L4 LB tier** behind ECMP. It has two NICs: the **external** one
  (`GEN_NIC_EXT`), attached to the border router, where client requests arrive; the **internal** one
  (`GEN_NIC_INT`), attached to the zone router, where your encapsulated packets leave.
- It fronts **services** (chat, video, web, ...), each identified by (VIP, port, protocol), each with its own set of
  identical **replicas**, identified by their IPv4 address.
- Forwarding is **IP-in-IP with direct server return**: you wrap each client packet in an outer IPv4 header (protocol
  4, source your internal address, destination the replica) and send it to the zone router. Replicas answer the
  clients directly, so you only see client-to-replica traffic.
- The harness also plays **the clients** (they retransmit unanswered SYNs and reconnect lost connections), **the
  replicas** (they fail, drain, come back, get added) and **the other LB instances** of the tier (they join and
  leave, and flows move between instances).

The goal: every packet of a flow reaches the same replica, new flows go only to active replicas, everything that is
not traffic for a service is dropped, and all of this as fast as possible within a memory budget.

## Your LB

You define `struct lb` (your LB's state) and `struct pkt_state` (your per-packet scratch state), and register your
functions in an `lb_ops_t`:

| Member | Called | You do |
|---|---|---|
| `state_size` | | `sizeof(pkt_state_t)` |
| `init(g, cfg)` | **twice** by `gen_open` | allocate and set up an LB instance (the first is your main LB, the second the [copy](#the-second-copy-and-the-other-lb-instances)) |
| `on_event(lb, ev)` | on both instances, for each replica event | update your view of the replicas |
| `hash` | by your loop, through `gen_hash` | parse the packets, find their service, hash their flow |
| `lookup` | through `gen_lookup` | find the packets' flows in your flow table |
| `compute_replica` | through `gen_replica` | choose a replica (consistent hashing), leave it in the state |
| `insert` | through `gen_insert` | store new flows in your flow table |
| `replica_of(s)` | by the harness, on the copy | return the replica's IPv4 address that `compute_replica` left in `s` |

`cfg` (`gen_config_t`) gives the services and their **initial** replicas, the MAC addresses of your NICs and of the
routers, your internal address, a secret `seed` for your hash, your `mem_budget`, and the whole `params` string.
Addresses and ports are in **host byte order** in `cfg` and in events (in packets they are in network order, as
always). `cfg->services` is never updated: replica changes come only through `on_event`.

The four stages work on a **batch** of up to 64 packets: `p[0..n)` and their states `s[0..n)`. A **mask** is a
64-bit bitmap over the batch, bit *i* standing for `p[i]` and `s[i]`. Passed in, bit *i* means "process `p[i]`";
returned, it means "the stage succeeded on `p[i]`" (`lookup`: found; `compute_replica`: chose a replica; `insert`:
stored). A stage must return only bits it was given. Bits at or above `n` mean nothing anywhere: the harness
ignores them.

Your main loop, in outline (see `lb/main.c` for a complete stub):

```c
gen_t *g = gen_open(argv[1], argc > 2 ? argv[2] : "", &ops);
if (!g) return 1;                                  /* the reason is on stderr */
lb_t *lb = gen_lb(g);                              /* your main instance */
pkt_t *p[64];
pkt_state_t s[64];
while (gen_running(g)) {
    uint8_t n = gen_rx_batch(g, GEN_NIC_EXT, p, 64);
    uint64_t all = n == 64 ? ~0ull : (1ull << n) - 1;
    gen_hash(g, lb, p, n, s, all);
    uint64_t ok = ...;                             /* the packets of a service; the others are dropped */
    uint64_t hit = gen_lookup(g, lb, p, n, s, ok);
    uint64_t got = gen_replica(g, lb, p, n, s, ok & ~hit);
    gen_insert(g, lb, p, n, s, got /* the new flows among them */);
    /* encapsulate hit | got, then: */
    gen_tx_batch(g, GEN_NIC_INT, p, n, hit | got);
    gen_drop(g, p, n, all & ~(hit | got));
}
gen_close(g);                                      /* writes the results */
```

The order and the use of the stages are yours (e.g. compute a replica only on a lookup miss), but **call your stages
through the wrappers** `gen_hash`, `gen_lookup`, `gen_replica` and `gen_insert`: they call your function with the
same arguments, then count per service from the masks passed and returned (index hits and misses, replica
computations, inserts stored and failed) and, with `profile=1`, time the call. They cost a few nanoseconds per call;
counting is fastest when you pass them the array `gen_rx_batch` filled. You may do work outside the stages, but it is
neither counted nor timed per stage.

- `gen_rx_batch(g, nic, p, max)` returns up to `max` (at most 64) of the oldest packets waiting in that NIC's
  receive ring (1,024 packets). It never blocks and may return 0.
- `gen_running(g)` returns 0 once the trace has ended, the rings are empty, and you have returned every packet (or
  1,000 calls went by without you returning the ones you still hold: they are leaks).
- `gen_param(g, key)` returns a parameter's value, or NULL: your own parameters live in the same string as the
  harness's.
- `gen_close(g)` writes the results and frees everything. It is called at exit if you forget.

Threads: in saturation and paced modes the harness calls the copy from its own thread. Keep all your state in your
`lb_t` (no mutable globals), and the two instances never interfere. Your loop runs on one thread; pin it with
`taskset` for measurements.

## Packets

- A packet is a `pkt_t`: `buf` points to a buffer of `GEN_BUFSIZE` (1,536) bytes; the frame starts at `buf + off`
  and is `len` bytes long, Ethernet header included. On receive, `off` is `GEN_HEADROOM` (20): the headroom is there
  for your outer header. `ts_ns` is the packet's time in the trace: **your LB's only clock** (for flow timeouts, for
  instance). Never change `buf`.
- Frames are full size: `len` and the IP total length are real. Only the headers are meaningful: payload bytes are
  unspecified, TCP checksums may be wrong; the IPv4 header checksum is correct. IPv4 headers are 20 bytes, never
  fragmented, with no VLAN tags.
- Not everything is IPv4 traffic for a service: ARP, IPv6 neighbour discovery and LLDP frames, BGP and BFD to your
  own address, port scans and pings of the VIPs, UDP to a TCP service. All of them must be dropped. A SYN flood
  targets a service: those SYNs belong to it, and forwarding them is correct.
- **Ownership**: a packet `gen_rx_batch` gives you is yours until it appears in the mask of **exactly one**
  `gen_tx_batch` or `gen_drop` call. Sending or dropping it twice, or a packet you do not own, is an error; a packet
  never returned is a leak.

**Encapsulation.** A frame you send on `GEN_NIC_INT` must be, from `buf + off` for `len` bytes:

- Ethernet: destination `cfg->zone_mac`, source `cfg->int_mac`, EtherType 0x0800;
- outer IPv4: version 4, IHL 5, protocol 4, source `cfg->int_ipv4`, destination the address of a replica of the
  packet's service, total length = inner total length + 20, TTL > 0, not a fragment, valid header checksum (TOS, ID
  and DF are free);
- then the inner IPv4 packet, its IP and TCP/UDP headers byte for byte as received (do not touch its TTL).

The usual way: write the new Ethernet and outer IP headers over the 20 bytes of headroom and the old Ethernet
header, then `off -= 20`, `len += 20`; the inner packet does not move. A frame that fails a check is **malformed**
(`malformed_REASON` rows of the results tell you which check: `buf_changed`, `out_of_buffer`, `len`, `eth_dst`,
`eth_src`, `ethertype`, `outer_ip_header`, `outer_proto`, `outer_src`, `outer_dst`, `outer_totlen`,
`outer_checksum`, `outer_ttl`, `inner_changed`, `wrong_nic`).

## Replica events

`on_event(lb, ev)` tells an instance that, at trace time `ev->ts_ns`, the replica `ev->ipv4` of service
`ev->service` was:

| Event | Meaning | New flows | Existing flows |
|---|---|---|---|
| `REPLICA_ADD` | added (new, or back after a removal) | yes | |
| `REPLICA_DRAIN` | draining (rolling upgrade): it will be removed soon | **no** | keep them there |
| `REPLICA_DOWN` | down (failed, or removed) | no | they are lost: send their packets elsewhere |
| `REPLICA_UP` | active again (repaired, or no longer draining) | yes | |

Events come from `gen_rx_batch` itself, on your thread: when the next packet comes after an event, the call first
calls `on_event` on your main instance and on the copy, then returns the packets after it. A batch never spans an
event. A failure is reported only after a health-check delay (3 s by default): in the meantime, packets sent to the
failed replica are lost, and you cannot know. A new flow sent to a draining replica, or to a down one after you were
told, is a **misplaced** flow.

## The second copy and the other LB instances

The tier changes too, and you are never told:

- **An LB joins**: some of your flows move to it; their packets stop arriving (their entries in your table go stale).
- **An LB leaves**: some of its flows move to you; their packets start arriving mid-flow, without a SYN.
- At time 0, the flows already in progress (the run starts in steady state) also arrive mid-flow, without a SYN.

For those flows, the replica they must keep is the one the **other instance** chose when the flow started. The
other instances run your own code: that is the **copy**, the second instance `init` returned. The copy receives
every replica event, never any traffic, and, at the start time of each flow that started elsewhere, the harness
builds the flow's first packet (a SYN), calls the copy's `hash` then `compute_replica` (mask 1, a zeroed
`pkt_state_t`), then your `replica_of`. Hence:

- `compute_replica` must work on its own, without `lookup` having run before it;
- what it answers there is that flow's replica: a packet of that flow that reaches your main instance later, with no
  entry in your table, must go to the same replica, even though the replica set may have changed since;
- if the copy's `compute_replica` does not return bit 0, or its answer is not an active replica, the flow counts as
  misplaced (`copy_failures`).

The copy's calls are not counted or timed, and its memory is accounted separately.

## Memory

Allocate everything your LB keeps with `gen_alloc`, free it with `gen_mfree`:

```c
void *gen_alloc(gen_t *g, size_t bytes, size_t align, int huge);
void  gen_mfree(gen_t *g, void *ptr);
```

- `align` is a power of 2, or 0 or 1 for `alignof(max_align_t)`. A block counts its size rounded up to `align`.
- Each instance has its own account. Your main instance may not hold more than `cfg->mem_budget` bytes (32 MB unless
  the scenario says otherwise): beyond it, `gen_alloc` returns NULL (`alloc_failed`). The results report your
  **peak**. Allocations made in `init`, `on_event` or a stage are charged to the instance they run on; those made by
  your own loop code, to the main instance.
- `huge == 0`: the block is never backed by huge pages, even when the kernel's transparent huge pages are in
  "always" mode. `huge != 0`: the harness asks the kernel to back it with huge pages (2 MB with 4 KB base pages,
  32 MB with 16 KB pages on some ARM kernels); it is aligned to a huge page and counts as whole huge pages against the
  budget. The kernel may still refuse: the results report the huge-page size (`huge_page_size`), the bytes of your
  huge blocks live at the end of the run (`huge_memory`) and how many of them were really in huge pages
  (`huge_backed`). `GEN_HUGETLB=1` in the environment tries reserved huge pages (`MAP_HUGETLB`) first (not in a
  `make debug` build, which has guard pages around huge blocks that `MAP_HUGETLB` mappings cannot have).
- Double frees and pointers not from `gen_alloc` are errors. Whatever you have not freed at the end is freed by the
  harness.
- A `make debug` build fences your blocks, and AddressSanitizer reports (as `use-after-poison`, at your line) an access:
  - just outside a block: up to 16 bytes plus an eighth of its size (1 KB at most) past a normal block, and as much
    before a block of up to 64 KB; for a larger or a huge block, up to the next page or huge page and then a page of
    guard on each side;
  - to a block after `gen_mfree`, even after you allocate another one of the same size: freed blocks are held back
    (the last 2048, or 256 MB) before their memory is reused.

  An access much farther away (a long overflow that lands in another block) or to a block freed long ago may go
  unnoticed. The release build (`make`) has none of this.

## Parameters

The parameter string is `key=value` pairs separated by commas. Empty items are skipped (an empty autotuner line,
a trailing comma). Each key may appear only once: a key given twice makes `gen_open` fail, with
`duplicate key: KEY` on standard error. These keys are the harness's; every other key is yours (read them with
`gen_param`). A bad value of a harness key makes `gen_open` fail.

| Key | Values | Meaning |
|---|---|---|
| `mode` | `functional` (default), `saturation`, `paced`, `bench` | see [modes](#modes) |
| `load`, `sat_pps` | e.g. `0.8`, `5M` | paced mode: arrivals average `load × sat_pps` packets/s (`sat_pps` required, `load` 0.5 by default) |
| `profile` | 0 or 1 | time every stage call and every `on_event` |
| `series` | seconds | also write the counters per interval of trace time |
| `flowlog` | path | also write one line per flow |
| `seed` | number | the traffic seed (overrides the scenario's) |
| `web_tunnel` | 0 or 1 | the [bonus](#bonus-web-in-a-two-way-tunnel) mode |
| `results` | path | the results file (default `results.csv`) |
| `pool_mb` | ≥ 8 | the packet buffer pool, MB (default 256) |
| `bench_keys`, `bench_dist`, `bench_phase`, `bench_hit` | | bench mode only |

`build/autotune` and `config.params` produce your keys for the contest (table sizes, batch sizes, ...).

## Modes

All modes start in steady state; nothing is measured during the warm-up (66 s of trace time by default), but
correctness is tracked throughout.

- **functional** (default): virtual time only, nothing is ever dropped, the harness waits for your loop. The results
  depend only on the scenario, the seed and your LB's decisions. Use it for correctness.
- **saturation**: the receive ring is kept full by a generator thread; a slower LB just takes longer. The harness
  measures the real time from the end of the warm-up to the end of the trace. A `gen_rx_batch` call on the external
  NIC that returns fewer packets than asked (for another reason than an event or the end) is a **generator stall**:
  any stall makes the speed measurement invalid (`speed_valid`). Ask for 64 packets per call.
- **paced**: the trace is replayed in real time, scaled so that packets arrive at `load × sat_pps` per second on
  average, bursts preserved; `ts_ns` still carries trace time. Packets that find the ring full are dropped. The
  latency of each packet (from its arrival to the `gen_tx_batch` call that sends it) gives the median, p99 and p99.9.
  `pace_valid` is 0 when the generator could not keep up with the schedule.
- **bench**: your flow table alone. One synthetic TCP service, no time, no events, no noise; the scenario is
  optional (`gen_open(NULL, "mode=bench,...", &ops)`). The packets arrive on the external NIC and carry `bench_keys` distinct flows (default 1M;
  `k` and `M` accepted), distributed as `bench_dist` (`uniform`, `sequential_ports`, `nat`), in `bench_phase` `fill`
  (each packet a new flow, until the keys run out) or `mixed` (each packet repeats a flow already sent with
  probability `bench_hit`, 0.9 by default). Run your stages as usual; stage times are always reported. Load factors
  and displacements are yours to compute from your own table.

The generator thread can be pinned with `GEN_GEN_CPU=<core>` in the environment (pin your loop to another core with
`taskset`).

## Outcomes

| Outcome | Definition |
|---|---|
| Broken flow | packets of one flow reached two different replicas, and the first one was not down |
| Lost flow | an established flow whose replica went down: not your fault, reported apart |
| Misplaced flow | a new flow sent to a draining replica, or to a down one after you were told; also a copy failure |
| Index miss | a packet passed to `lookup`, not found, and not the first packet of its flow that you see: not an error by itself |
| Drop | a packet you dropped, or that found the ring full (paced mode); per service, and `none` for frames of no service |
| Misforwarded | a frame of no service, sent instead of dropped |
| Malformed | a sent frame that fails the checks of [packets](#packets) |
| Leak | a packet received and never sent or dropped |

- A flow whose SYN went to a replica that was down, and whose retransmitted SYN then reached a live replica, is not
  broken. Dropping a legitimate packet is a drop, not a broken flow.
- **Effective rate** (saturation): the packets of legitimate flows handled correctly from start to end (never
  broken, never misplaced, nothing malformed; lost flows and flood flows excluded), divided by the measured real
  time. A run with any leak, misforwarded frame or malformed frame is **disqualified** for the contest.

## The results file

A CSV file with one value per row: `scope,key,value`. Scopes:

- `run`: the whole run: `mode`, `seed`, `trace_hash`, `events`, `leaks`, `errors_sent_twice`, `errors_not_owned`,
  `errors_stage_mask`, `peak_memory`, `huge_page_size`, `huge_memory`, `huge_backed`, `memory_budget`,
  `alloc_failed`, `errors_double_free`, `errors_unknown_free`, `misforwarded`, `disqualified`, `real_time_s`; in
  saturation mode `raw_pps`, `effective_pps`, `useful_fraction`, `generator_stalls`, `speed_valid`; in paced mode
  `latency_median_ns`, `latency_p99_ns`, `latency_p999_ns`, `pace_valid` and the rates;
- `all` and one scope per service (by name): `pkts_rx`, `pkts_tx`, `pkts_dropped`, `pkts_ring_dropped`, `malformed`
  and `malformed_REASON`, `broken_flows`, `lost_flows`, `misplaced_flows`, `index_hits`, `index_misses`,
  `replica_computations`, `inserts_stored`, `inserts_failed`, flow counts (`flows`, `flows_midflow`,
  `flows_moved_in`, `flows_moved_out`, `syn_retransmissions`, `reconnects`, ...), flood counts (`flood_rx`,
  `flood_fwd`, `flood_dropped`, `flood_misplaced`), `copy_failures`, `effective_pkts`;
- `none`: the frames of no service (`pkts_rx`, `pkts_dropped`, `misforwarded`, ...);
- with `profile=1` (always in bench mode), one scope per stage (`hash`, `lookup`, `compute_replica`, `insert`,
  `on_event`): `calls`, `pkts`, `time_ns`, `ns_per_call`, `ns_per_pkt`;
- in bench mode, `bench`: `keys`, `dist`, `phase`, `hit`, `pkts`, `new_keys`, `repeats`.

Counts start after the warm-up. Other files: `RESULTS.series.csv` with `series=` (one row per interval and service:
packets, drops, errors, wrapper counts), `RESULTS.latency.csv` in paced mode (the latency histogram), and the
`flowlog=` file (`saddr,daddr,sport,dport,proto,service,first_s,last_s,end,broken,lost,misplaced,origin`, with `end`
one of `fin`, `rst`, `vanish`, `lost`, `moved`, `gaveup`, `alive`). A summary line also goes to standard error
(`GEN_QUIET=1` silences it).

## Testing and debugging

- `make test` checks that `build/autotune` prints one well-formed line, then runs `build/lb` on the small scenarios
  of `tests/` and checks their expected outcomes, written in each scenario's comments (`# expect SCOPE,KEY = VALUE`):
  frames well formed, no leaks, noise all dropped, replica churn alone breaks no flow and misplaces none, failures
  and drains avoided once known, agreement with the copy. `BONUS=1 make test` adds the bonus scenario. The stub LB
  fails most of them: it drops everything.
- Errors (sending a packet twice, a packet you do not own, a stage returning bits it was not given, bad frees) print
  a message and are counted; `GEN_ABORT=1` in the environment makes the first one abort, so that a debugger or the
  sanitizers show you where it happened.
- `make debug` and `tests/run.sh build-debug` run everything under AddressSanitizer and UndefinedBehaviorSanitizer.
- Write your own scenarios (see `scenarios/FORMAT.md`): a few services, a few replicas, a short duration, the events you want
  to test, at given times.

## Bonus: web in a two-way tunnel

With `web_tunnel=1`, the web replicas answer through your LB. Their replies arrive on `GEN_NIC_INT` with `off = 0`:
Ethernet (from `zone_mac` to `int_mac`), outer IPv4 (from the replica to `int_ipv4`, protocol 4), then the server's
packet (from VIP:port to the client). Decapsulate them and send them on `GEN_NIC_EXT`: Ethernet from `cfg->ext_mac`
to `cfg->border_mac`, then the inner packet unchanged, `off`/`len` describing exactly that (`off += 20`,
`len -= 20`). A reply exists only for a request you forwarded correctly. The results count `replies_rx`,
`replies_fwd`, `replies_dropped` and `replies_malformed` per service.

## CRC32C

`gen/crc32c.h` gives `crc32c_u64`, `crc32c_u32` and `crc32c_u8` (`uint32_t crc, value → uint32_t`), using the CRC
instructions of x86-64 (SSE 4.2) or AArch64: both architectures give the same results. No inversion is applied:
start from `~0` and invert at the end for the standard CRC32C value. Mix in `cfg->seed` so that your hash cannot be
predicted.

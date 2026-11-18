# Scenario format

A scenario is a plain text file, one statement per line. `#` starts a comment. Words are separated by spaces;
arguments are `key=value` pairs, in any order. Global statements may appear anywhere; service statements apply to
the last `service` line above them.

```
# a small example
duration 5min
warmup 66

service web
  vip 198.51.100.30 port 443 proto tcp
  replicas 48 10.3.0.1
  model web flows=5000 lifetime_median=300ms
  upgrade start=120 interval=40 drain=20 groups=8
  failures rate=1/min repair=2min
  scale_out at=480 add=25%

noise arp=10 scan=50 udp=50
lb join at=100 fraction=0.2
```

## Values

| Kind | Syntax | Examples |
|---|---|---|
| time | a number, then `ns`, `us`, `ms`, `s` (default), `min` or `h` | `66`, `300ms`, `2min`, `1h` |
| rate (per second) | a number, optional `k` or `M`, then optional `/s`, `/min` or `/h` | `50`, `1.5k`, `1M`, `3/min` |
| number | a number, optional `k` or `M` | `5000`, `500k` |
| size | a number, optional `K`, `M` or `G`, optional `B` | `32M`, `1GB` |
| address | dotted IPv4 | `10.3.0.1` |
| MAC | six hex bytes | `02:00:00:00:00:01` |

## Global statements

| Statement | Default | Meaning |
|---|---|---|
| `duration T` | 600 s | length of the trace |
| `warmup T` | 66 s | nothing is measured before (correctness is tracked throughout); must be shorter than the duration |
| `health_delay T` | 3 s | a failed replica is reported (`REPLICA_DOWN`) this long after it fails |
| `seed N` | 1 | traffic seed (the `seed` parameter overrides it) |
| `ext_mac`, `int_mac`, `border_mac`, `zone_mac` `MAC` | `02:00:00:00:00:01`, `…:00:02`, `…:01:fe`, `…:02:fe` | the LB's NICs and the routers' interfaces |
| `int_addr A` | 10.255.0.1 | the LB's internal address (the outer source) |
| `lb_addr A` | 192.0.2.1 | the LB's own address on the external side (BGP, BFD and ARP noise use it) |
| `mem_budget S` | 32M | the LB's memory budget (`gen_alloc`) |
| `clients nat_share=X nat_addrs=N` | 0.3, 1000 | the share of connections from clients behind NAT addresses (sequential source ports), and the number of NAT addresses |
| `lb join at=T fraction=F` | | an LB instance joins the tier: each live flow of this LB moves to it with probability F |
| `lb leave at=T fraction=F` | | an LB instance leaves: F × `flows` of each service's flows in progress on it move to this LB, mid-flow |
| `noise KIND=RATE ...` | none | frames that belong to no service (see below) |

At most 16 LB-tier events. The LB is never told about them.

## Services

| Statement | Meaning |
|---|---|
| `service NAME` | starts a service (at most 15; names: letters, digits, `_`, `-`; not `all` or `none`) |
| `vip A port N proto tcp` | its address, port and protocol (pairs may be split over several lines); the traffic models are TCP |
| `replicas N A` | N initial replicas, at consecutive addresses from A |
| `model TYPE k=v ...` | the workload model and its parameters (below) |
| `param k=v ...` | more parameters of the model (after `model`) |
| `upgrade start=T interval=T drain=T groups=N` | rolling upgrade: defaults interval 10 s, drain 5 s, groups 8 |
| `failures rate=R repair=T` | random failures: R per second over the whole run; repaired after T (never without `repair`) |
| `scale_out at=T add=N` or `add=P%` | N new replicas (or P % of the initial ones) at T |
| `event T OP A` | an explicit replica event, OP = `add`, `drain`, `down`, `remove` or `up` |
| `flood start=T duration=T rate=R` | a SYN flood against this service |

Replica churn, in detail:
- **Rolling upgrade**: the initial replicas are split in `groups` slices. Slice k is drained at `start + k × interval`
  (`REPLICA_DRAIN`), then, `drain` later, removed (it goes down, `REPLICA_DOWN` at once) and replaced by as many new
  replicas (`REPLICA_ADD`, fresh addresses after the service's range).
- **Failures**: at random times (Poisson, rate R) a random replica among those up and active fails (never the last
  one); `REPLICA_DOWN` comes `health_delay` later; after `repair` it comes back (`REPLICA_UP`).
- **Explicit events**: `add` (a new address, or one that was removed), `drain`, `down` (a failure: reported after
  `health_delay`), `remove` (removed at once), `up` (back from a failure or a drain). An event that makes no sense at
  that time (e.g. draining a replica that is down) is an error when the scenario is loaded.
- A replica's established flows are **lost** when it goes down (failure or removal); their clients reconnect.

## Models

Every model has a `flows` parameter: the number of concurrent connections (for one LB instance). The run starts
in steady state with that many connections in progress. New connections arrive at the rate given by Little's law:

> new connections per second = `flows` / mean lifetime

Common TCP behaviour: a new connection sends a SYN, one RTT later the ACK and the TLS ClientHello, another RTT later
the TLS Finished, then the model's packets; it ends with a FIN (one RTT before the end of its lifetime, then the ACK
of the server's FIN), a RST, or nothing (the client vanishes), with the shares `vanish` and `rst`. The RTT of each
connection is uniform in [`rtt_min`, `rtt_max`]. An unanswered SYN (dropped, malformed, or sent to a replica that is
down) is retransmitted after 1 s, then doubling, 6 times. A client whose connection was lost reconnects within
`reconnect`.

### `chat`: personal messaging (WebSocket push)

| Parameter | Default | Meaning |
|---|---|---|
| `flows` | 500k | concurrent connections |
| `lifetime` | 1h | mean lifetime (exponential) |
| `keepalive` | 60s | the client's keepalive period (data, then the ACK of the answer) |
| `keepalive_jitter` | 0.1 | ± share of the period |
| `event_interval` | 2min | mean time between messages (exponential), half sent, half received |
| `typing` | 2 | typing indicators per message and side, about 1.5 s apart |
| `photo_share` | 0.1 | share of messages carrying a photo |
| `photo_pkts` | 100 | mean data segments of a photo upload (half as many ACKs for a download) |
| `vanish`, `rst` | 0.05, 0 | shares of connections ending without FIN, with RST |
| `rtt_min`, `rtt_max` | 10ms, 150ms | |
| `reconnect` | 10s | lost clients reconnect within this delay (reconnect storm) |

Packets per connection and second ≈ 2 / `keepalive` + (`typing` + 1) / `event_interval` + photo segments.

### `video`: streaming with ON/OFF segment downloads

| Parameter | Default | Meaning |
|---|---|---|
| `flows` | 2500 | concurrent connections |
| `lifetime_median`, `lifetime_sigma` | 8min, 0.29 | lognormal lifetime |
| `segment` | 4s | a segment request every period (±10 %) |
| `on_share` | 0.25 | share of the period spent downloading (a dense run of ACKs) |
| `pps` | 200 | mean packets per second per connection |
| `sync`, `sync_jitter` | 0, 50ms | with `sync=1`, the players' periods align on a global grid (± jitter) |
| `vanish`, `rst` | 0.01, 0 | |
| `rtt_min`, `rtt_max` | 10ms, 100ms | |
| `reconnect` | 2s | |

Mean lifetime = `lifetime_median` × exp(`lifetime_sigma`² / 2).

### `web`: web and API traffic

| Parameter | Default | Meaning |
|---|---|---|
| `flows` | 5000 | concurrent connections |
| `lifetime_median`, `lifetime_sigma` | 300ms, 1.0 | lognormal lifetime (at least 20 ms) |
| `pkts_min`, `pkts_max` | 8, 30 | packets per connection, uniform |
| `h1_share`, `h1_max` | 0.3, 6 | share of page loads opening 2 to `h1_max` parallel connections from consecutive source ports (HTTP/1.1); the others open one |
| `upload_share` | 0.05 | share of the middle packets that are 1,500-byte uploads |
| `vanish`, `rst` | 0.05, 0.10 | |
| `rtt_min`, `rtt_max` | 5ms, 80ms | (at most a fifth of the lifetime) |
| `reconnect` | 1s | |
| `server_delay`, `reply_pkts` | 2ms, 3 | bonus `web_tunnel=1`: the replica's replies to each request |

Mean lifetime = `lifetime_median` × exp(`lifetime_sigma`² / 2). Page loads per second = connections per second /
(1 − `h1_share` + `h1_share` × (2 + `h1_max`) / 2).

### `simple`: for tests

| Parameter | Default | Meaning |
|---|---|---|
| `flows` | 100 | concurrent connections |
| `lifetime` | 1s | every connection lives exactly this long |
| `pkts` | 10 | packets per connection, SYN and FIN included, evenly spaced |
| `size` | 100 | IP total length of the data packets |

## Noise and flood

`noise KIND=RATE ...` (frames per second), all arriving on the external NIC and all to be dropped:

| Kind | Frame |
|---|---|
| `arp` | ARP request (broadcast) from the border router for an address of the LB's /24 |
| `nd` | IPv6 neighbour solicitation from the router's link-local address |
| `lldp` | LLDP from the router |
| `bgp` | BGP keepalive (TCP 179) to `lb_addr` |
| `bfd` | single-hop BFD control packet (UDP 3784) to `lb_addr` |
| `scan` | SYN to another port of a VIP |
| `ping` | ICMP echo request to a VIP |
| `udp` | UDP (a QUIC Initial) to a TCP service's VIP and port |

`flood start=T duration=T rate=R` (in a service): spoofed SYNs to the service (random source address and port, no
options), never retransmitted or completed. They belong to the service: forwarding them is correct, but they never
count in the effective rate.

## Published scenarios

| File | Content |
|---|---|
| `chat.scn`, `video.scn`, `web.scn` | each service alone, at the figures above, with a rolling upgrade, failures and a scale-out |
| `mixed.scn` | the three services, the same replica churn, one LB join (before the replica changes) and one LB leave (after) |
| `mixed_flood.scn` | the contest scenario: `mixed.scn` plus a 1 M SYN/s flood on web for 30 s |
| `events.scn` | one service with 500 replicas and a high rate of replica events (event cost) |
| `test/*.scn` | small scenarios for the tests |

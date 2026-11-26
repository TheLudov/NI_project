#!/bin/sh
# E3, replica selection (Atieh): how many points per replica does each service need?
#
#   scripts/e3/run_e3.sh [SEEDS]          (default 5; about 25 minutes on our VM)
#
# 1. build/e3_ring (scripts/e3/e3_ring.c): the ring alone, for chat (64 replicas), video (32) and web (48), with
#    v = 1, 10, 100, 1000 points per replica and 7 secret seeds: balance by flows (from the ring's arcs, and with
#    each service's real number of concurrent flows), share of the flows moved when a replica is added or removed
#    against the ideal 1/N, memory, and the time of one ring lookup.                       -> build/e3/ring.csv
# 2. build/lb_e3: our LB built with -DE3_STATS=1 (per-replica counters in lb/main.c, off in the normal build), run in
#    functional mode on scripts/e3/e3_{chat,video,web}.scn (each service alone, no replica or LB churn, so that the
#    replica set is fixed), for each v and SEEDS traffic seeds (a seed also changes the LB's secret seed, hence the
#    ring): balance by flows and by packets over the run, and per second of trace time; how often a ring lookup is
#    paid (replica computations / packets) and compute_replica's profile (calls, packets, time; profile=1). The
#    harness calls the stage once per batch, even with no packet to compute, so its ns_per_pkt mixes a cost per
#    call with the cost per lookup: e3_plot.py takes the difference with v = 1 instead.     -> build/e3/balance.csv
# 3. the contest scenario (mixed_flood.scn) once, with config.params as it is: how often each service pays a ring
#    lookup when the flood is on.                                                         -> build/e3/contest.csv
# 4. scripts/e3/e3_plot.py: the figures and the summary table.   -> build/e3/*.png, summary.txt, machine.txt
#
# Everything else in the configuration comes from config.params (minus ring_v and the per-service ring_v_<VIP>,
# since this script sets v for every service).
# Pin it if you can: GEN_GEN_CPU=1 taskset -c 0 scripts/e3/run_e3.sh
set -eu
cd "$(dirname "$0")/../.."
SEEDS=${1:-5}
case "$(uname -m)" in
    x86_64) AF=-msse4.2; LIB=gen/x86_64/libgen.a ;;
    aarch64|arm64) AF=-march=armv8-a+crc; LIB=gen/aarch64/libgen.a ;;
    *) echo "run_e3.sh: unsupported machine $(uname -m)" >&2; exit 1 ;;
esac
CFLAGS="-std=c11 -O2 -Wall -Wextra $AF -Isrc -Igen"
OUT=build/e3
mkdir -p "$OUT/raw"
{   # the machine, for the report (the brief asks for it next to every figure)
    uname -srm
    if command -v lscpu > /dev/null; then
        lscpu | grep -E '^(Model name|CPU\(s\)|Hypervisor vendor|Virtualization type|L1d|L2|L3)' || true
    fi
    grep -E '^MemTotal' /proc/meminfo 2> /dev/null || true
    cc --version | head -n 1
    for f in /sys/devices/system/cpu/cpufreq/boost /sys/devices/system/cpu/intel_pstate/no_turbo; do
        [ -r "$f" ] && echo "$f: $(cat "$f")"
    done
    echo "pinning: ${GEN_GEN_CPU:+generator thread on CPU $GEN_GEN_CPU, }loop CPU mask $(taskset -p $$ 2> /dev/null | sed 's/.*: //')"
} > "$OUT/machine.txt" 2>&1 || true
cc $CFLAGS scripts/e3/e3_ring.c src/ring.c src/tabhash.c -o build/e3_ring
cc $CFLAGS -DE3_STATS=1 src/*.c lb/main.c "$LIB" -lpthread -lm -o build/lb_e3

echo "1/4 the ring alone (7 seeds)" >&2
build/e3_ring 7 > "$OUT/ring.csv"

echo "2/4 the LB, each service alone, no churn ($SEEDS seeds)" >&2
CONF=$(tr ',' '\n' < config.params | grep -v '^ring_v[=_]' | grep -v '^$' | paste -sd, -)
TABLE="ft_buckets=109226,bucket_size=8,max_depth=2,dfs_budget=64"
echo "service,v,seed,secret_seed,active,flows,pkts,flows_maxavg,pkts_maxavg,sec_median,sec_p95,pkts_rx,replica_computations,miss_fraction,cr_calls,cr_pkts,cr_time_ns" > "$OUT/balance.csv"
for seed in $(seq 1 "$SEEDS"); do
    for v in 1 10 100 1000; do
        for svc in chat video web; do
            tag="$svc.v$v.s$seed"
            GEN_QUIET=1 build/lb_e3 "scripts/e3/e3_$svc.scn" \
                "$TABLE,$CONF,ring_v=$v,seed=$seed,profile=1,results=$OUT/raw/$tag.csv" 2> "$OUT/raw/$tag.log"
            awk -v svc="$svc" -v v="$v" -v seed="$seed" -v res="$OUT/raw/$tag.csv" '
                /^E3 / { for (i = 2; i <= NF; i++) { split($i, kv, "="); e[kv[1]] = kv[2] } }
                END {
                    while ((getline line < res) > 0) {
                        split(line, f, ",")
                        if (f[1] == "all" && f[2] == "pkts_rx") rx = f[3]
                        if (f[1] == "all" && f[2] == "replica_computations") rc = f[3]
                        if (f[1] == "compute_replica" && f[2] == "calls") cc = f[3]
                        if (f[1] == "compute_replica" && f[2] == "pkts") cp = f[3]
                        if (f[1] == "compute_replica" && f[2] == "time_ns") ct = f[3]
                    }
                    printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%.6f,%s,%s,%s\n", svc, v, seed, e["secret_seed"],
                        e["active"], e["flows"], e["pkts"], e["flows_maxavg"], e["pkts_maxavg"], e["sec_median"],
                        e["sec_p95"], rx, rc, rx ? rc / rx : 0, cc, cp, ct
                }' "$OUT/raw/$tag.log" >> "$OUT/balance.csv"
            echo "  $tag done" >&2
        done
    done
done

echo "3/4 the contest scenario once, with config.params as it is (how often each service looks up its ring)" >&2
GEN_QUIET=1 build/lb_e3 scenarios/mixed_flood.scn "$TABLE,$(tr -d '\n' < config.params),results=$OUT/contest.csv" \
    > "$OUT/raw/contest.out" 2> "$OUT/raw/contest.log"

echo "4/4 figures" >&2
python3 scripts/e3/e3_plot.py "$OUT"

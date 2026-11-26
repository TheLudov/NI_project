#!/bin/sh
# E2, the hash function: is simple tabulation hashing fast and well spread? Tabulation (hashfn=tab) against CRC32C
# (hashfn=crc), on the same machine, in the same session, alternating.
#
#   scripts/e2_hash.sh                 # everything, ~10 min; then: python3 scripts/e2_plot.py build/e2
#   STEPS="speed spread" scripts/e2_hash.sh
#
# Steps (STEPS, default all), each writing build/e2/STEP.csv:
#   speed   ns per hash, alone (thr: independent hashes, lat: dependent chain, locate: + bucket indices and
#           signature), with K = 0..8 random cache lines read per hash from a 256 KB (L2) or 8 MB (L3) buffer: the
#           cache-space question, measured (scripts/e2_hash/e2.c speed)
#   spread  bucket fill on realistic 5-tuples: chi-square, fullest bucket, collisions (e2.c spread)
#   fill    load factor at the first failed insert, displacements per insert (e2.c fill): E1's load factor
#   bench   the LB itself in the harness's bench mode: stage times per packet, tab against crc, per key distribution
#   seed    do the hash's collisions survive a change of seed? (scripts/e2_hash/seedcheck.c, writes seed.txt)
#
# Environment: SEEDS [1 2 3 4 5], REPS [5], CPU [2] (the measuring loop), GEN_CPU [4] (the harness's generator),
#   TABLE [the autotuner's output, else ft_buckets=65536,bucket_size=10,max_depth=5,dfs_budget=64],
#   BENCH_KEYS [500k], FLOWLOG [build/e2/flows_mixed.csv, made from scenarios/mixed.scn if missing].
#
# Author: Ludovic
set -eu
LC_ALL=C; export LC_ALL
cd "$(dirname "$0")/.."

STEPS=${STEPS:-speed spread fill bench seed}
SEEDS=${SEEDS:-1 2 3 4 5}
REPS=${REPS:-5}
CPU=${CPU:-2}
GEN_CPU=${GEN_CPU:-4}
BENCH_KEYS=${BENCH_KEYS:-500k}
OUT=build/e2
FLOWLOG=${FLOWLOG:-$OUT/flows_mixed.csv}
mkdir -p "$OUT" build-e2

# ---- build: build/lb as the Makefile does, and the E2 tool with the same flags plus -DFT_STATS=1 ----
make -s all
case $(uname -m) in
    x86_64)        archflags=-msse4.2;           genlib=gen/x86_64/libgen.a ;;
    aarch64|arm64) archflags=-march=armv8-a+crc; genlib=gen/aarch64/libgen.a ;;
    *) echo "unsupported architecture $(uname -m)" >&2; exit 1 ;;
esac
# shellcheck disable=SC2046
${CC:-cc} -std=c11 -O2 -Wall -Wextra $archflags -Isrc -Igen -DFT_STATS=1 \
    $(find src -name '*.c') scripts/e2_hash/e2.c "$genlib" -lpthread -lm -o build-e2/e2
${CC:-cc} -std=c11 -O2 -Wall -Wextra $archflags -Isrc -Igen scripts/e2_hash/seedcheck.c src/tabhash.c \
    -o build-e2/seedcheck

# ---- the table layout, and the machine (report it with every figure) ----
if [ -z "${TABLE:-}" ]; then
    TABLE=$(build/autotune 2>/dev/null || true)
    [ -n "$TABLE" ] || TABLE="ft_buckets=65536,bucket_size=10,max_depth=5,dfs_budget=64"
fi
tval() { echo "$TABLE" | tr ',' '\n' | sed -n "s/^$1=//p"; }
NB=$(tval ft_buckets); BS=$(tval bucket_size); DEPTH=$(tval max_depth); BUDGET=$(tval dfs_budget)
NB=${NB:-65536}; BS=${BS:-10}; DEPTH=${DEPTH:-5}; BUDGET=${BUDGET:-64}
seeds_csv=$(echo "$SEEDS" | tr -s ' ' ',')
{
    echo "date: $(date -Iseconds)"
    echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
    lscpu | grep -E '^(L1d|L2|L3)' | sed 's/  */ /g'
    echo "virt: $(systemd-detect-virt 2>/dev/null || true)"
    echo "governor: $(cat /sys/devices/system/cpu/cpu$CPU/cpufreq/scaling_governor 2>/dev/null || echo NA)"
    echo "boost: $(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || echo NA)"
    echo "compiler: $(${CC:-cc} --version | head -n 1)"
    echo "table: $TABLE"
    echo "pinning: loop on cpu $CPU, generator on cpu $GEN_CPU"
} > "$OUT/machine.txt"
cat "$OUT/machine.txt" >&2

has() { case " $STEPS " in *" $1 "*) return 0 ;; esac; return 1; }

# real 5-tuples: the flows of the mixed scenario (functional mode, about a minute)
dists="uniform,seqports,nat,mixed"
if has spread || has fill; then
    if [ ! -s "$FLOWLOG" ]; then
        echo "flowlog of scenarios/mixed.scn -> $FLOWLOG" >&2
        base=$(tr ',' '\n' < config.params | grep -v '^[[:space:]]*$' | paste -sd, -)
        GEN_QUIET=1 build/lb scenarios/mixed.scn "$TABLE${base:+,$base},flowlog=$FLOWLOG,results=$OUT/flowlog_run.csv" \
            > /dev/null
    fi
    dists="$dists,flowlog"
fi

if has speed; then
    echo "speed ($REPS reps)" >&2
    first=1
    for kb in 256 8192; do
        taskset -c "$CPU" build-e2/e2 speed reps="$REPS" pollute_kb=$kb nb="$NB" bs="$BS" 2>/dev/null |
            if [ $first = 1 ]; then cat; else tail -n +2; fi
        first=0
    done > "$OUT/speed.csv"
fi

if has spread; then
    echo "spread (seeds $SEEDS)" >&2
    taskset -c "$CPU" build-e2/e2 spread nb="$NB" bs="$BS" seeds="$seeds_csv" dists="$dists" \
        flowlog="$FLOWLOG" 2>/dev/null > "$OUT/spread.csv"
fi

if has fill; then
    echo "fill (seeds $SEEDS)" >&2
    taskset -c "$CPU" build-e2/e2 fill nb="$NB" bs="$BS" depth="$DEPTH" budget="$BUDGET" seeds="$seeds_csv" \
        dists="$dists" flowlog="$FLOWLOG" 2>/dev/null > "$OUT/fill.csv"
fi

if has bench; then
    echo "bench: the LB in the harness's bench mode ($BENCH_KEYS keys, seeds $SEEDS)" >&2
    # config.params without the keys this step sets itself
    base=$(tr ',' '\n' < config.params | grep -v -E '^(hashfn|ft_buckets|bucket_size|max_depth|dfs_budget)=' |
           grep -v '^[[:space:]]*$' | paste -sd, -)
    echo "hashfn,dist,seed,hash_ns,lookup_ns,replica_ns,insert_ns,total_ns" > "$OUT/bench.csv"
    i=0
    for seed in $SEEDS; do
        for dist in uniform sequential_ports nat; do
            # alternate which hash function goes first
            if [ $((i % 2)) = 0 ]; then order="tab crc"; else order="crc tab"; fi
            i=$((i + 1))
            for h in $order; do
                res="$OUT/bench_${h}_${dist}_$seed.results.csv"
                GEN_QUIET=1 GEN_GEN_CPU=$GEN_CPU taskset -c "$CPU" build/lb "" \
                    "mode=bench,bench_phase=mixed,bench_keys=$BENCH_KEYS,bench_dist=$dist,seed=$seed,hashfn=$h,$TABLE${base:+,$base},results=$res" \
                    > /dev/null 2>&1
                # ns per packet received, per stage (hash runs on every packet)
                awk -F, -v h="$h" -v d="$dist" -v s="$seed" '
                    $2=="time_ns" {t[$1]=$3} $1=="hash" && $2=="pkts" {n=$3}
                    END {printf "%s,%s,%s,%.3f,%.3f,%.3f,%.3f,%.3f\n", h, d, s, t["hash"]/n, t["lookup"]/n,
                         t["compute_replica"]/n, t["insert"]/n,
                         (t["hash"]+t["lookup"]+t["compute_replica"]+t["insert"])/n}' "$res" >> "$OUT/bench.csv"
            done
        done
    done
fi

if has seed; then
    echo "seed" >&2
    build-e2/seedcheck > "$OUT/seed.txt"
    cat "$OUT/seed.txt" >&2
fi

echo "done: $OUT/*.csv; figures and summary: python3 scripts/e2_plot.py $OUT" >&2

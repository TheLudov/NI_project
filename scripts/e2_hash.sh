#!/bin/sh
# E2, the hash function: is simple tabulation hashing fast and well spread? Simple tabulation (hashfn=tab) against
# CRC32C (hashfn=crc), on the same machine, in the same session, alternating between them.
#
#   scripts/e2_hash.sh                        # every step, a few minutes; then: python3 scripts/e2_plot.py build/e2
#   STEPS="speed spread" scripts/e2_hash.sh   # only some steps
#
# Steps (STEPS, all by default), each writing build/e2/STEP.csv:
#   speed   ns per hash, alone and with ft_locate (e2 speed)
#   spread  how evenly keys fill the buckets: uniform, sequential ports, NAT (e2 spread)
#   fill    the load factor at the first failed insert, E1's measure, with each hash (e2 fill)
#   bench   the LB itself in the harness's bench mode, hashfn=tab against hashfn=crc: ns per packet per stage
#   seed    do the hash's collisions survive a change of seed? (seedcheck.c, writes seed.txt)
#   perf    perf stat of "e2 one tab|crc|none": instructions, cycles, L1 misses, branch misses (perf.csv)
#   asm     the assembly of tabhash.c (gcc -O2 -S): tabhash.s, and the instructions of each hash function (asm.txt)
#
# The test suite runs first: the brief counts no timing before it passes.
#
# Environment: CPU [2] (the measuring loop), GEN_CPU [4] (the harness's generator thread), BENCH_KEYS [500k],
#   TABLE [the autotuner's output, else ft_buckets=65536,bucket_size=10,max_depth=5,dfs_budget=64].
#
# Author: Ludovic
set -eu
LC_ALL=C; export LC_ALL
cd "$(dirname "$0")/.."

STEPS=${STEPS:-speed spread fill bench seed perf asm}
CPU=${CPU:-2}
GEN_CPU=${GEN_CPU:-4}
BENCH_KEYS=${BENCH_KEYS:-500k}
OUT=build/e2
mkdir -p "$OUT" build-e2

# ---- build: build/lb as the Makefile does, and the E2 tools with the Makefile's flags (-std=c11 -O2 and the CRC
# instruction), linked with src/ so that they call the LB's own functions ----
make -s test                    # correctness first (set -e: a failure stops here)
case $(uname -m) in
    x86_64)        archflags=-msse4.2;           genlib=gen/x86_64/libgen.a ;;
    aarch64|arm64) archflags=-march=armv8-a+crc; genlib=gen/aarch64/libgen.a ;;
    *) echo "unsupported architecture $(uname -m)" >&2; exit 1 ;;
esac
# shellcheck disable=SC2046
${CC:-cc} -std=c11 -O2 -Wall -Wextra $archflags -Isrc -Igen \
    $(find src -name '*.c') scripts/e2_hash/e2.c "$genlib" -lpthread -lm -o build-e2/e2
${CC:-cc} -std=c11 -O2 -Wall -Wextra $archflags -Isrc -Igen scripts/e2_hash/seedcheck.c src/tabhash.c \
    -o build-e2/seedcheck

# ---- the table layout: the autotuner's, as in the contest ----
if [ -z "${TABLE:-}" ]; then
    TABLE=$(build/autotune 2>/dev/null || true)
    [ -n "$TABLE" ] || TABLE="ft_buckets=16384,bucket_size=8,max_depth=5,dfs_budget=64"
fi
tval() { echo "$TABLE" | tr ',' '\n' | sed -n "s/^$1=//p"; }     # one key's value: tval bucket_size -> 10
NB=$(tval ft_buckets); BS=$(tval bucket_size); DEPTH=$(tval max_depth); BUDGET=$(tval dfs_budget)
LAYOUT="${NB:-65536} ${BS:-10} ${DEPTH:-5} ${BUDGET:-64}"          # e2's arguments: nb bs depth budget

# ---- the machine: reported with every figure, as the brief asks ----
{
    echo "date: $(date -Iseconds)"
    echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
    lscpu | grep -E '^(L1d|L2|L3)' | sed 's/  */ /g'
    l3=/sys/devices/system/cpu/cpu$CPU/cache/index3
    echo "L3 of cpu $CPU: $(cat $l3/size 2>/dev/null), shared with cpus $(cat $l3/shared_cpu_list 2>/dev/null)"
    echo "memory: $(grep MemTotal /proc/meminfo | sed 's/MemTotal: *//')"
    echo "virt: $(systemd-detect-virt 2>/dev/null || true)"
    echo "governor: $(cat /sys/devices/system/cpu/cpu"$CPU"/cpufreq/scaling_governor 2>/dev/null || echo NA)"
    echo "boost: $(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || echo NA)"
    echo "compiler: $(${CC:-cc} --version | head -n 1)"
    echo "table: $TABLE"
    echo "pinning: loop on cpu $CPU, generator on cpu $GEN_CPU"
} > "$OUT/machine.txt"
cat "$OUT/machine.txt" >&2

has() { case " $STEPS " in *" $1 "*) return 0 ;; esac; return 1; }

# ---- speed, spread, fill: e2, pinned to CPU. e2 opens the harness for its allocator: GEN_GEN_CPU keeps the
# harness's thread, if it starts one, off the measuring core; GEN_QUIET silences its summary line ----
for step in speed spread fill; do
    if has $step; then
        echo "$step" >&2
        # shellcheck disable=SC2086
        GEN_QUIET=1 GEN_GEN_CPU=$GEN_CPU taskset -c "$CPU" build-e2/e2 $step $LAYOUT > "$OUT/$step.csv"
    fi
done

# ---- bench: the whole LB in the harness's bench mode, once per hash function, key set and seed ----
if has bench; then
    echo "bench: the LB in the harness's bench mode ($BENCH_KEYS keys, seeds 1-5)" >&2
    # config.params without the keys this step sets itself (gen_open rejects a key given twice)
    base=$(tr ',' '\n' < config.params | grep -v -E '^(hashfn|ft_buckets|bucket_size|max_depth|dfs_budget)=' |
           grep -v '^[[:space:]]*$' | paste -sd, -)
    echo "hashfn,dist,seed,hash_ns,lookup_ns,replica_ns,insert_ns,total_ns" > "$OUT/bench.csv"
    i=0
    for seed in 1 2 3 4 5; do
        for dist in uniform sequential_ports nat; do
            if [ $((i % 2)) = 0 ]; then order="tab crc"; else order="crc tab"; fi     # alternate which goes first
            i=$((i + 1))
            for h in $order; do
                res="$OUT/bench_${h}_${dist}_$seed.results.csv"
                GEN_QUIET=1 GEN_GEN_CPU=$GEN_CPU taskset -c "$CPU" build/lb "" \
                    "mode=bench,bench_phase=mixed,bench_keys=$BENCH_KEYS,bench_dist=$dist,seed=$seed,hashfn=$h,$TABLE${base:+,$base},results=$res" \
                    > /dev/null 2>&1
                # results rows are scope,key,value: each stage's time_ns, divided by the packets of the hash stage
                awk -F, -v h="$h" -v d="$dist" -v s="$seed" '
                    $2=="time_ns" {t[$1]=$3} $1=="hash" && $2=="pkts" {n=$3}
                    END {printf "%s,%s,%s,%.3f,%.3f,%.3f,%.3f,%.3f\n", h, d, s, t["hash"]/n, t["lookup"]/n,
                         t["compute_replica"]/n, t["insert"]/n,
                         (t["hash"]+t["lookup"]+t["compute_replica"]+t["insert"])/n}' "$res" >> "$OUT/bench.csv"
            done
        done
    done
fi

# ---- seed ----
if has seed; then
    echo "seed" >&2
    build-e2/seedcheck > "$OUT/seed.txt"
    cat "$OUT/seed.txt" >&2
fi

# ---- perf: hardware counters of one hash function per process (user space only: no root needed) ----
if has perf; then
    echo "perf" >&2
    echo "hashfn,rep,hashes,event,count" > "$OUT/perf.csv"
    for rep in 1 2 3 4 5; do
        case $rep in 1|4) order="tab crc none" ;; 2|5) order="crc none tab" ;; *) order="none tab crc" ;; esac
        for h in $order; do
            hashes=$(GEN_QUIET=1 GEN_GEN_CPU=$GEN_CPU perf stat -x, -o "$OUT/perf_${h}_$rep.txt" \
                         -e instructions:u,cycles:u,L1-dcache-load-misses:u,branch-misses:u \
                         taskset -c "$CPU" build-e2/e2 one "$h" | tail -n 1 | cut -d, -f2)
            # perf -x, lines: count,unit,event,...
            awk -F, -v h="$h" -v r="$rep" -v n="$hashes" '$1 ~ /^[0-9]+$/ {sub(/:u$/, "", $3); print h "," r "," n "," $3 "," $1}' \
                "$OUT/perf_${h}_$rep.txt" >> "$OUT/perf.csv"
        done
    done
fi

# ---- asm: what the compiler made of the two hash functions (the Makefile's flags) ----
if has asm; then
    echo "asm" >&2
    ${CC:-cc} -std=c11 -O2 -Wall -Wextra $archflags -Isrc -Igen -S src/tabhash.c -o "$OUT/tabhash.s"
    # instructions of a function: the lines after its label, up to its ret, that are not labels or directives
    for f in tabhash_flow crc_flow; do
        awk -v f="$f" '$0 == f ":" {on = 1; next}
                       on && /^[ \t]+[a-z]/ && !/^[ \t]+\./ {n++; ops[$1]++}
                       on && /^[ \t]+ret/ {on = 0}
                       END {printf "%s: %d instructions:", f, n; for (o in ops) printf " %s x%d", o, ops[o]; print ""}' \
            "$OUT/tabhash.s"
    done > "$OUT/asm.txt"
    cat "$OUT/asm.txt" >&2
fi

echo "done: $OUT/*.csv; figures and summary: python3 scripts/e2_plot.py $OUT" >&2

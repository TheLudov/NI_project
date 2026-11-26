#!/bin/sh
# E5, the three services together: one configuration for the shared LB; the SYN flood on web, its effect on chat and
# video, and the defence (syn_guard, lb/main.c).
#
#   scripts/e5_shared.sh                         # every step; then: python3 scripts/e5_plot.py build/e5
#   STEPS="stress sat paced" scripts/e5_shared.sh
#
# Variants (the autotuner's table + config.params, minus the keys a variant sets):
#   noflood          mixed.scn,       every SYN stored half-open (the LB before E5)
#   noflood_guard    mixed.scn,       syn_guard=$GUARD: does the defence cost anything without a flood?
#   flood            mixed_flood.scn, no defence
#   flood_stateless  mixed_flood.scn, syn_insert=0: no SYN ever stored (the naive defence)
#   flood_guard      mixed_flood.scn, syn_guard=$GUARD: at most GUARD SYNs/s stored per service, the rest stateless
#
# Steps (STEPS, all by default); rows of every CSV: variant,seed,scope,key,value
#   func    functional mode, 5 seeds x the variants, in parallel (functional results do not depend on the machine)
#   occ     functional, seed 1: occupancy of the shared table every second -> occ_VARIANT.csv
#   stress  functional, 3 seeds: flood and flood_guard with web's half-open timeout at 1 s (to_half 1000 ms): the
#           flood then holds ~1 M half-open entries, more than the table's slots
#   sat     saturation mode, pinned, 5 seeds, flood and flood_guard alternated: raw and effective rate
#   paced   the variant with the higher median effective rate, at load 0.8 of its median raw rate, 5 seeds
#
# Functional runs whose results file exists are skipped (the harness writes it only at the end of a run): an
# interrupted campaign resumes where it stopped. Delete build/e5/runs to start over (always after changing the code).
#
# Environment: CPU [2] (the LB loop), GEN_CPU [4] (the generator thread), JOBS [10] (parallel functional runs),
#   GUARD [50000], TABLE [build/autotune's output].
#
# Author: Ludovic
set -eu
LC_ALL=C; export LC_ALL
cd "$(dirname "$0")/.."

STEPS=${STEPS:-func occ stress sat paced}
CPU=${CPU:-2}
GEN_CPU=${GEN_CPU:-4}
JOBS=${JOBS:-10}
GUARD=${GUARD:-50000}
SEEDS="1 2 3 4 5"
OUT=build/e5
mkdir -p "$OUT/runs"

make -s
make -s test                    # correctness first (set -e: a failure stops here)

TABLE=${TABLE:-$(build/autotune 2>/dev/null)}
# config.params without the keys the variants set (gen_open rejects a key given twice)
base=$(tr ',' '\n' < config.params | grep -v -E '^(syn_insert|syn_guard)=' | grep -v '^[[:space:]]*$' | paste -sd, -)
P="$TABLE,$base"
STRESS="to_half_ms_198.51.100.30=1000"

{
    echo "date: $(date -Iseconds)"
    echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
    lscpu | grep -E '^(L1d|L2|L3)' | sed 's/  */ /g'
    echo "memory: $(grep MemTotal /proc/meminfo | sed 's/MemTotal: *//')"
    echo "virt: $(systemd-detect-virt 2>/dev/null || true)"
    echo "governor: $(cat /sys/devices/system/cpu/cpu"$CPU"/cpufreq/scaling_governor 2>/dev/null || echo NA)"
    echo "boost: $(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null || echo NA)"
    echo "compiler: $(${CC:-cc} --version | head -n 1)"
    echo "params: $P"
    echo "guard: $GUARD SYN/s per service; stress: $STRESS"
    echo "pinning: loop on cpu $CPU, generator on cpu $GEN_CPU (sat, paced)"
} > "$OUT/machine.txt"

has() { case " $STEPS " in *" $1 "*) return 0 ;; esac; return 1; }
scn_of() { case $1 in noflood*) echo scenarios/mixed.scn ;; *) echo scenarios/mixed_flood.scn ;; esac; }
keys_of() { case $1 in *_stateless) echo "syn_insert=0" ;; *_guard) echo "syn_guard=$GUARD" ;; esac; }

# run_parallel JOBFILE: one run per line, "scenario|params" (params end with results=PATH)
run_parallel() {
    tr '\n' '\0' < "$1" | xargs -0 -P "$JOBS" -I{} sh -c '
        IFS="|"; set -- $1
        res=${2##*results=}
        [ -f "$res" ] && exit 0
        GEN_QUIET=1 build/lb "$1" "$2" > /dev/null 2>&1 || echo "run failed: $res" >&2' _ {}
}
# collect OUT PREFIX VARIANTS: the runs PREFIX_VARIANT_SEED.csv, prefixed with variant,seed, into OUT
collect() {
    echo "variant,seed,scope,key,value" > "$1"
    for v in $3; do for seed in $SEEDS; do
        f=$OUT/runs/$2_${v}_$seed.csv
        [ -f "$f" ] && awk -v l="$v,$seed" '{print l "," $0}' "$f" >> "$1"
    done; done
    return 0
}

if has func; then
    echo "func" >&2
    V="noflood noflood_guard flood flood_stateless flood_guard"
    for seed in $SEEDS; do for v in $V; do
        k=$(keys_of $v)
        echo "$(scn_of $v)|$P${k:+,$k},seed=$seed,series=1,results=$OUT/runs/func_${v}_$seed.csv"
    done; done > "$OUT/func.jobs"
    run_parallel "$OUT/func.jobs"
    collect "$OUT/func.csv" func "$V"
fi

if has occ; then
    echo "occ" >&2
    for v in noflood flood flood_guard; do
        k=$(keys_of $v)
        echo "$(scn_of $v)|$P${k:+,$k},seed=1,occ_every_s=1,occ_file=$OUT/occ_$v.csv,results=$OUT/runs/occ_$v.csv"
    done > "$OUT/occ.jobs"
    run_parallel "$OUT/occ.jobs"
fi

if has stress; then
    echo "stress: $STRESS" >&2
    for seed in 1 2 3; do for v in flood flood_guard; do
        k=$(keys_of $v)
        echo "scenarios/mixed_flood.scn|$P,$STRESS${k:+,$k},seed=$seed,series=1,results=$OUT/runs/stress_${v}_$seed.csv"
    done; done > "$OUT/stress.jobs"
    run_parallel "$OUT/stress.jobs"
    collect "$OUT/stress.csv" stress "flood flood_guard"
fi

if has sat; then
    echo "sat" >&2
    for seed in $SEEDS; do
        if [ $((seed % 2)) = 1 ]; then order="flood flood_guard"; else order="flood_guard flood"; fi
        for v in $order; do
            k=$(keys_of $v); res=$OUT/runs/sat_${v}_$seed.csv
            GEN_QUIET=1 GEN_GEN_CPU=$GEN_CPU taskset -c "$CPU" build/lb scenarios/mixed_flood.scn \
                "$P${k:+,$k},seed=$seed,mode=saturation,results=$res" > /dev/null 2>&1
            echo "  $v seed $seed: $(grep -E '^run,(raw_pps|effective_pps|speed_valid),' "$res" | cut -d, -f2,3 | tr '\n' ' ')" >&2
        done
    done
    collect "$OUT/sat.csv" sat "flood flood_guard"
fi

# median of a run-scope key over the sat runs of a variant
sat_med() { awk -F, -v v="$1" -v k="$2" '$1==v && $3=="run" && $4==k {print $5}' "$OUT/sat.csv" | sort -g |
            awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'; }

if has paced; then
    [ -f "$OUT/sat.csv" ] || { echo "paced: needs sat.csv (the sat step)" >&2; exit 1; }
    best=flood
    awk -v a="$(sat_med flood_guard effective_pps)" -v b="$(sat_med flood effective_pps)" 'BEGIN {exit !(a > b)}' &&
        best=flood_guard
    sat=$(sat_med $best raw_pps | awk '{printf "%d", $1}')
    echo "paced: $best at 0.8 x $sat pps" >&2
    k=$(keys_of $best)
    for seed in $SEEDS; do
        res=$OUT/runs/paced_${best}_$seed.csv
        GEN_QUIET=1 GEN_GEN_CPU=$GEN_CPU taskset -c "$CPU" build/lb scenarios/mixed_flood.scn \
            "$P${k:+,$k},seed=$seed,mode=paced,load=0.8,sat_pps=$sat,results=$res" > /dev/null 2>&1
        echo "  seed $seed: $(grep -E '^run,(latency_median_ns|latency_p99_ns|pace_valid),' "$res" | cut -d, -f2,3 | tr '\n' ' ')" >&2
    done
    collect "$OUT/paced.csv" paced "$best"
fi

echo "done: $OUT/*.csv; figures and summary: python3 scripts/e5_plot.py $OUT" >&2

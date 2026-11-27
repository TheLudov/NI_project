#!/bin/sh
# The report's final numbers (Atieh): our effective rate on the contest scenario (mixed_flood.scn, with the
# autotuner's line and config.params, as the contest runs it), its two factors (raw packet rate and useful
# fraction), and the 99th-percentile latency at 80 % of that saturation rate (paced mode).
#   saturation: our configuration, the same without syn_guard, and with batch 64 and pf_bucket 0; SEEDS seeds each
#   paced: load 0.8 of our configuration's median saturation rate, SEEDS seeds
#   scripts/final/run_final.sh          results in build/final/ (a run whose results exist is skipped)
#   LB_CPU, GEN_CPU: the cores of the loop and the generator (default 2 and 4); SEEDS (default 5); SCN (scenario)
set -eu
cd "$(dirname "$0")/../.."
OUT=build/final
LB_CPU=${LB_CPU:-2}
GEN_CPU=${GEN_CPU:-4}
SEEDS=${SEEDS:-5}
SCN=${SCN:-scenarios/mixed_flood.scn}
mkdir -p "$OUT"
make -s
# pset LIST KEY VALUE: LIST with KEY set to VALUE (gen_open rejects a key given twice)
pset() { printf '%s\n' "$1" | tr ',' '\n' | grep -v "^$2=" | grep -v '^$' | { cat; echo "$2=$3"; } | paste -sd, -; }
P="$(build/autotune 2> /dev/null),$(tr -d '\n' < config.params)"
echo "$P" > "$OUT/params.txt"
{ uname -srm; LC_ALL=C lscpu 2> /dev/null | grep -E '^(Model name|CPU\(s\)|Hypervisor vendor)' || true; } > "$OUT/machine.txt"

run() {    # TAG PARAMS
    [ -s "$OUT/$1.csv" ] && return 0
    GEN_QUIET=1 GEN_GEN_CPU=$GEN_CPU taskset -c "$LB_CPU" build/lb "$SCN" "$2,results=$OUT/$1.part" \
        > /dev/null 2> "$OUT/$1.log" || true
    if [ -s "$OUT/$1.part" ]; then
        if [ -f "$OUT/$1.part.latency.csv" ]; then mv "$OUT/$1.part.latency.csv" "$OUT/$1.latency.csv"; fi
        mv "$OUT/$1.part" "$OUT/$1.csv"
        echo "  $1" >&2
    else
        echo "  $1 FAILED (see $OUT/$1.log)" >&2
    fi
}
median() {  # KEY FILES...: the median of run,KEY over the files
    key=$1; shift
    grep -h "^run,$key," "$@" 2> /dev/null | cut -d, -f3 | sort -g | awk '{a[NR] = $1} END {if (NR) print a[int((NR + 1) / 2)]}'
}

# pdel LIST KEY: LIST without KEY
pdel() { printf '%s\n' "$1" | tr ',' '\n' | grep -v "^$2=" | grep -v '^$' | paste -sd, -; }
# The variants: ours (config.params as it is), the same without the SYN guard, and the same with the batch and
# prefetching of before (batch 64, pf_bucket 0)
V_final="$P"
V_noguard=$(pdel "$P" syn_guard)
V_base=$(pset "$(pset "$P" batch 64)" pf_bucket 0)
VARIANTS="final noguard base"

echo "saturation on $SCN" >&2
for seed in $(seq 1 "$SEEDS"); do
    for v in $VARIANTS; do
        eval "Q=\$V_$v"
        run "sat_${v}_s$seed" "$Q,seed=$seed,mode=saturation"
    done
done
SAT=$(median raw_pps $(ls "$OUT"/sat_final_s*.csv 2> /dev/null | grep -v latency))
if [ -n "$SAT" ]; then
    echo "paced at 80 % of $SAT packets/s (our configuration)" >&2
    for seed in $(seq 1 "$SEEDS"); do
        run "paced80_final_s$seed" "$P,seed=$seed,mode=paced,load=0.8,sat_pps=$SAT"
    done
fi

for v in $VARIANTS; do
    f=$(ls "$OUT"/sat_${v}_s*.csv 2> /dev/null | grep -v latency || true)
    [ -n "$f" ] || continue
    echo "$v: effective $(median effective_pps $f) = raw $(median raw_pps $f) x useful $(median useful_fraction $f);" \
         "speed_valid $(grep -h '^run,speed_valid,1' $f | wc -l)/$(echo $f | wc -w)"
done | tee "$OUT/summary.txt"
p=$(ls "$OUT"/paced80_final_s*.csv 2> /dev/null | grep -v latency || true)
if [ -n "$p" ]; then
    echo "final, paced 80 %: p99 latency $(median latency_p99_ns $p) ns, median $(median latency_median_ns $p) ns" \
         "(medians of the runs); pace_valid $(grep -h '^run,pace_valid,1' $p | wc -l)/$(echo $p | wc -w)" | tee -a "$OUT/summary.txt"
fi

#!/bin/sh
# E1, prefetching (Atieh): where does prefetching help, and how much does it gain?
#
#   scripts/e1_prefetch.sh [REPS] [KEYS] [HIT] [TABLE]
#
# Runs build/lb in the harness's bench mode (the flow table alone, synthetic keys, stage times always reported) for
# each prefetching variant of $VARIANTS, REPS times each (default 5) with a different traffic seed each time. The
# variants alternate within each repetition (brief, "Measuring properly"), each repetition starting from the next
# one. Prints, per variant, the median and the spread (min-max) of the four stages' time per packet received, then
# the per-stage medians. Raw results go to build/e1_prefetch/.
#
#   KEYS   distinct flows (bench_keys, default 500k: ~75 % of a 655k-slot table)
#   HIT    share of packets of a flow already seen (bench_hit, default 0.9)
#   TABLE  the flow table's parameters (default: the autotuner's output if it prints one, else dev's)
#
# A variant is PBE:D: pf_pkt, pf_bucket, pf_entry (each 0 or 1) and pf_dist D (0: whole batch first; D > 0: D
# packets ahead), e.g. VARIANTS="000:0 011:4" scripts/e1_prefetch.sh.
#
# Pin it: the loop on one core, the harness's generator on another (taskset on Linux), e.g.
#   GEN_GEN_CPU=1 taskset -c 0 scripts/e1_prefetch.sh
set -eu
cd "$(dirname "$0")/.."
REPS=${1:-5}
KEYS=${2:-500k}
HIT=${3:-0.9}
TABLE=${4:-}
VARIANTS=${VARIANTS:-"000:0 100:0 010:0 001:0 111:0 100:4 010:4 011:2 011:4 011:8 011:16 111:4 111:8"}
if [ -z "$TABLE" ]; then
    TABLE=$(build/autotune 2>/dev/null || true)
    [ -n "$TABLE" ] || TABLE="ft_buckets=65536,bucket_size=10,max_depth=5,dfs_budget=64"
fi
OUT=build/e1_prefetch
rm -rf "$OUT"
mkdir -p "$OUT"
NV=$(echo "$VARIANTS" | wc -w)

echo "table: $TABLE; bench_keys=$KEYS bench_hit=$HIT; $REPS repetitions of $NV variants" >&2
r=0
while [ "$r" -lt "$REPS" ]; do
    k=0
    while [ "$k" -lt "$NV" ]; do
        j=$(( (k + r) % NV + 1 ))
        v=$(echo "$VARIANTS" | tr -s ' ' '\n' | sed -n "${j}p")
        a=$(echo "$v" | cut -c1); b=$(echo "$v" | cut -c2); c=$(echo "$v" | cut -c3); d=$(echo "$v" | cut -d: -f2)
        tag=$(echo "$v" | tr ':' '_')
        GEN_QUIET=1 build/lb "" "mode=bench,bench_phase=mixed,bench_keys=$KEYS,bench_hit=$HIT,seed=$((r + 1)),$TABLE,pf_pkt=$a,pf_bucket=$b,pf_entry=$c,pf_dist=$d,results=$OUT/pf$tag.r$r.csv" >/dev/null 2>"$OUT/pf$tag.r$r.log"
        k=$((k + 1))
    done
    r=$((r + 1))
done

# per run: stage ns per packet received (hash is called on every packet); then median and spread per variant
for v in $VARIANTS; do
    tag=$(echo "$v" | tr ':' '_')
    for f in "$OUT"/pf$tag.r*.csv; do
        awk -F, '$2=="time_ns" && ($1=="hash"||$1=="lookup"||$1=="compute_replica"||$1=="insert") {t[$1]=$3}
                 $1=="hash" && $2=="pkts" {n=$3}
                 END {printf "%.2f %.2f %.2f %.2f %.2f\n", (t["hash"]+t["lookup"]+t["compute_replica"]+t["insert"])/n,
                      t["hash"]/n, t["lookup"]/n, t["compute_replica"]/n, t["insert"]/n}' "$f"
    done | sort -n | awk -v v="$v" '{tot[NR]=$1; h[NR]=$2; l[NR]=$3; c[NR]=$4; i[NR]=$5}
        END {m=int((NR+1)/2); split(v, x, ":");
             printf "pf_pkt=%s pf_bucket=%s pf_entry=%s pf_dist=%-2s  total %6.1f ns/pkt (%5.1f-%5.1f) | hash %5.1f lookup %5.1f replica %5.1f insert %5.1f\n",
             substr(v,1,1), substr(v,2,1), substr(v,3,1), x[2], tot[m], tot[1], tot[NR], h[m], l[m], c[m], i[m]}'
done

#!/bin/sh
# E4 (Atieh): each service alone, end to end. Runs what E4 measures, then scripts/e4/e4_plot.py, into build/e4/.
#
#   functional  correctness, in functional mode, two runs at a time. The error table of each service on its
#               published scenario; chat under replica churn alone (chat.scn), LB churn alone and both; the idle
#               timeout (chat: shorter than the keepalive, video, web); a table too small for the service with each
#               insert fallback (ins_fallback=0, 1, 2). The LB is built with -DE4_STATS=1: each run's log has, per
#               second of trace time, the table's entries by state and the misses, new flows, failed inserts and
#               evictions.
#   saturation  packet rate of each service (2-minute traces) for batch = 8, 16, 32, 64; and video and web with a
#               table sized for them instead of the autotuner's.
#   events      events.scn in saturation mode with profile=1, with and without its replica events, and with them
#               coalesced (ev_coalesce=1): the time the LB spends in on_event, and in hash, where coalesced events are
#               applied.
#   paced       latency (median, p99) and RX drops at 50, 80 and 95 % of each saturation rate, for each batch size;
#               and events.scn with its events, coalesced or not, and without them, at the same loads. Needs the
#               saturation and events
#               results, and a machine where the harness keeps the pace (pace_valid = 1): not a small VM.
#
#   scripts/e4/run_e4.sh [RUN_GROUPS] [SEEDS]   RUN_GROUPS: comma-separated (default functional,saturation,events); SEEDS: 5
#   LB_CPU, GEN_CPU: the cores the loop and the generator are pinned to in saturation and paced mode (default 0, 1).
#   A run whose results exist (build/e4/raw/TAG.csv) is skipped: an interrupted run resumes where it stopped.
#   About 35 min for the default groups on our 2-vCPU VM; paced about 40 min more.
set -eu
SELF="$(cd "$(dirname "$0")" && pwd)/$(basename "$0")"
cd "$(dirname "$0")/../.."
OUT=build/e4
LB_CPU=${LB_CPU:-0}
GEN_CPU=${GEN_CPU:-1}

# One run: TAG SCENARIO PARAMS PIN BINARY. PIN=1 pins the loop and the generator (saturation and paced).
one() {
    [ -s "$OUT/raw/$1.csv" ] && return 0
    if [ "$4" = 1 ]; then
        GEN_QUIET=1 GEN_GEN_CPU=$GEN_CPU taskset -c "$LB_CPU" "$5" "$OUT/scn/$2.scn" "$3,results=$OUT/raw/$1.part" \
            > /dev/null 2> "$OUT/raw/$1.log" || true
    else
        GEN_QUIET=1 "$5" "$OUT/scn/$2.scn" "$3,results=$OUT/raw/$1.part" > /dev/null 2> "$OUT/raw/$1.log" || true
    fi
    if [ -s "$OUT/raw/$1.part" ]; then
        [ -f "$OUT/raw/$1.part.series.csv" ] && mv "$OUT/raw/$1.part.series.csv" "$OUT/raw/$1.series.csv"
        [ -f "$OUT/raw/$1.part.latency.csv" ] && mv "$OUT/raw/$1.part.latency.csv" "$OUT/raw/$1.latency.csv"
        mv "$OUT/raw/$1.part" "$OUT/raw/$1.csv"
        echo "  $1" >&2
    else
        echo "  $1 FAILED (see $OUT/raw/$1.log)" >&2
    fi
}
if [ "${1:-}" = __one ]; then           # a functional job, from xargs below
    one "$2" "$3" "$4" 0 build/lb_e4
    exit 0
fi

RUN_GROUPS=${1:-functional,saturation,events}
SEEDS=${2:-5}
has() { case ",$RUN_GROUPS," in *",$1,"*) return 0 ;; *) return 1 ;; esac; }
# pset LIST KEY VALUE: the comma-separated LIST with KEY set to VALUE (gen_open rejects a key given twice)
pset() { printf '%s\n' "$1" | tr ',' '\n' | grep -v "^$2=" | grep -v '^$' | { cat; echo "$2=$3"; } | paste -sd, -; }

case "$(uname -m)" in
    x86_64) AF=-msse4.2; LIB=gen/x86_64/libgen.a ;;
    aarch64|arm64) AF=-march=armv8-a+crc; LIB=gen/aarch64/libgen.a ;;
    *) echo "run_e4.sh: unsupported machine $(uname -m)" >&2; exit 1 ;;
esac
mkdir -p "$OUT/raw" "$OUT/scn"
make -s
cc -std=c11 -O2 -Wall -Wextra $AF -Isrc -Igen -DE4_STATS=1 src/*.c lb/main.c "$LIB" -lpthread -lm -o build/lb_e4
{   # the machine, for the report (the brief asks for it next to every figure)
    uname -srm
    if command -v lscpu > /dev/null; then
        LC_ALL=C lscpu | grep -E '^(Model name|CPU\(s\)|Thread\(s\) per core|Hypervisor vendor|Virtualization type|L1d|L2|L3)' || true
    fi
    grep -E '^MemTotal' /proc/meminfo 2> /dev/null || true
    cc --version | head -n 1
    for f in /sys/devices/system/cpu/cpufreq/boost /sys/devices/system/cpu/intel_pstate/no_turbo; do
        [ -r "$f" ] && echo "$f: $(cat "$f")"
    done
    echo "pinning in saturation and paced mode: loop on CPU $LB_CPU, generator on CPU $GEN_CPU"
} > "$OUT/machine.txt" 2>&1 || true

# The configuration: the autotuner's line, then config.params, as the contest builds it
TABLE=$(build/autotune 2> /dev/null)
P="$TABLE,$(tr -d '\n' < config.params)"
echo "$P" > "$OUT/params.txt"
echo "configuration: $P" >&2

# Scenarios: the published ones, and variants of them
for s in chat video web events; do cp "scenarios/$s.scn" "$OUT/scn/$s.scn"; done
NOCHURN='!/^[ \t]*(upgrade|failures|scale_out)[ \t]/'
LBCHURN='{ print } /^warmup[ \t]/ { print "lb join at=100 fraction=0.2"; print "lb leave at=540 fraction=0.25" }'
awk "$NOCHURN" scenarios/chat.scn | awk "$LBCHURN" > "$OUT/scn/chat_lb.scn"     # LB churn alone
awk "$LBCHURN" scenarios/chat.scn > "$OUT/scn/chat_both.scn"                    # replica and LB churn
awk "$NOCHURN" scenarios/events.scn > "$OUT/scn/events_none.scn"                # events.scn without its events
for s in chat video web; do sed 's/^duration .*/duration 2min/' "scenarios/$s.scn" > "$OUT/scn/${s}_2m.scn"; done
sed 's/^duration .*/duration 5min/' scenarios/video.scn > "$OUT/scn/video_5m.scn"
sed 's/^duration .*/duration 100s/' scenarios/web.scn > "$OUT/scn/web_100s.scn"

seq_seeds() { seq 1 "$SEEDS"; }

if has functional; then
    echo "functional: the error table, chat churn, timeouts, table size and insert fallback" >&2
    for seed in $(seq_seeds); do
        Q="$P,seed=$seed,series=1"
        for s in chat video web events; do echo "f_out__${s}__published__s$seed $s $Q"; done
        echo "f_churn__chat__lb__s$seed chat_lb $Q"
        echo "f_churn__chat__both__s$seed chat_both $Q"
        for t in 30000 60000 120000; do echo "f_to__chat__est${t}__s${seed} chat $(pset "$Q" to_est_ms $t)"; done
        for t in 2000 5000; do echo "f_to__video__est${t}__s${seed} video $(pset "$Q" to_est_ms $t)"; done
        echo "f_to__web__est5000__s$seed web $(pset "$Q" to_est_ms_198.51.100.30 5000)"
        echo "f_to__web__fin500__s$seed web $(pset "$Q" to_fin_ms 500)"
        # web's own setting (ins_fallback_198.51.100.30 in config.params) overrides ins_fallback: set both
        for fb in 0 1 2; do
            echo "f_fb__chat__b40000f${fb}__s${seed} chat $(pset "$(pset "$Q" ft_buckets 40000)" ins_fallback $fb)"
            echo "f_fb__video__b256f${fb}__s${seed} video_5m $(pset "$(pset "$Q" ft_buckets 256)" ins_fallback $fb)"
        done
        for fb in 0 1 2 3; do
            echo "f_fb__web__b4096f${fb}__s${seed} web $(pset "$(pset "$(pset "$Q" ft_buckets 4096)" ins_fallback $fb)" ins_fallback_198.51.100.30 $fb)"
        done
    done > "$OUT/functional.jobs"
    # how web flows end (fin, rst, vanish, ...), from the harness's per-flow log of 100 s of web.scn
    echo "f_flows__web__100s__s1 web_100s $P,seed=1,flowlog=$OUT/raw/web_flows.part" >> "$OUT/functional.jobs"
    xargs -P 2 -n 3 sh "$SELF" __one < "$OUT/functional.jobs"
    if [ -f "$OUT/raw/web_flows.part" ]; then
        python3 - "$OUT/raw/web_flows.part" > "$OUT/raw/web_flow_ends.csv" <<'EOF'
import collections, csv, sys
ends = collections.Counter(r["end"] for r in csv.DictReader(open(sys.argv[1])))
print("end,flows")
for k, v in sorted(ends.items(), key=lambda kv: -kv[1]):
    print(f"{k},{v}")
EOF
        rm -f "$OUT/raw/web_flows.part"     # about 85 MB per 100 s: only the counts are kept
    fi
fi

if has saturation; then
    echo "saturation: packet rate per service and batch size; video and web with a smaller table" >&2
    for s in chat video web; do
        for b in 8 16 32 64; do
            for seed in $(seq_seeds); do
                one "s_batch__${s}__b${b}__s$seed" "${s}_2m" "$P,seed=$seed,batch=$b,mode=saturation" 1 build/lb
            done
        done
    done
    for seed in $(seq_seeds); do
        one "s_table__video__b1024__s$seed" video_2m "$(pset "$P" ft_buckets 1024),seed=$seed,mode=saturation" 1 build/lb
        one "s_table__web__b8192__s$seed" web_2m "$(pset "$P" ft_buckets 8192),seed=$seed,mode=saturation" 1 build/lb
    done
fi

if has events; then
    echo "events: time spent in on_event (saturation, profile=1), with and without the replica events" >&2
    for seed in $(seq_seeds); do
        for v in events events_none; do
            one "e_sat__${v}__s$seed" "$v" "$(pset "$P" ev_coalesce 0),seed=$seed,mode=saturation,profile=1" 1 build/lb
        done
        one "e_sat__events_coalesce__s$seed" events "$(pset "$P" ev_coalesce 1),seed=$seed,mode=saturation,profile=1" 1 build/lb
    done
fi

# median raw_pps of the saturation runs matching a glob (paced mode paces relative to it)
median_pps() {
    python3 - "$@" <<'EOF'
import csv, glob, statistics, sys
v = []
for f in glob.glob(sys.argv[1]):
    for r in csv.reader(open(f)):
        if len(r) >= 3 and r[0] == "run" and r[1] == "raw_pps":
            v.append(float(r[2]))
print(f"{statistics.median(v):.0f}" if v else "")
EOF
}

if has paced; then
    echo "paced: latency and RX drops at 50, 80, 95 % of the saturation rate" >&2
    for s in chat video web; do
        for b in 8 16 32 64; do
            SAT=$(median_pps "$OUT/raw/s_batch__${s}__b${b}__s*.csv")
            [ -n "$SAT" ] || { echo "  no saturation result for $s batch=$b: run the saturation group first" >&2; continue; }
            for load in 0.5 0.8 0.95; do
                for seed in $(seq_seeds); do
                    one "p__${s}__b${b}l${load}__s$seed" "${s}_2m" \
                        "$P,seed=$seed,batch=$b,mode=paced,load=$load,sat_pps=$SAT" 1 build/lb
                done
            done
        done
    done
    SAT=$(median_pps "$OUT/raw/e_sat__events__s*.csv")
    if [ -n "$SAT" ]; then
        for v in events events_none events_coalesce; do
            scn=$v; ev=0
            [ "$v" = events_coalesce ] && { scn=events; ev=1; }
            for load in 0.5 0.8 0.95; do
                for seed in $(seq_seeds); do
                    one "pe__${v}__l${load}__s$seed" "$scn" \
                        "$(pset "$P" ev_coalesce $ev),seed=$seed,mode=paced,load=$load,sat_pps=$SAT" 1 build/lb
                done
            done
        done
    else
        echo "  no events result: run the events group first" >&2
    fi
fi

python3 scripts/e4/e4_plot.py "$OUT"

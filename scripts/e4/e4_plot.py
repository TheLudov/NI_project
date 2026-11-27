#!/usr/bin/env python3
"""E4, each service alone (Atieh): figures and tables from scripts/e4/run_e4.sh's raw results.
    python3 scripts/e4/e4_plot.py build/e4
Reads build/e4/raw/TAG.csv (the harness's results; TAG = group__service__variant__sSEED), TAG.log (the E4 build's
per-second table counters, lines E4T), TAG.series.csv (the harness's per-second counters) and writes:
    e4_saturation.png  packet rate in saturation mode against the batch size, per service
    e4_web_table.png   web's table entries over time, by state: half-open, established, closing, expired
    e4_chat_storm.png  chat over time: new flows per second (the reconnect storms) and the table's entries
    e4_paced.png       latency (median, p99) against the load, per service and batch size (if paced runs exist)
    summary.txt        every table, as text, with the machine
    e4_tables.tex      the same tables for the report (LaTeX, booktabs)
Each number is the median over the seeds, with [min, max] where they differ. Needs matplotlib.
"""
import csv
import glob
import os
import statistics
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, FuncFormatter, NullLocator

OUT = sys.argv[1] if len(sys.argv) > 1 else "build/e4"
RAW = f"{OUT}/raw"
SERVICES = ["chat", "video", "web"]
BATCHES = [8, 16, 32, 64]
LOADS = ["0.5", "0.8", "0.95"]
BLUE, ORANGE, AQUA, YELLOW = "#257de0", "#f47230", "#00b894", "#f2a600"
TEXT, TEXT2, GRID = "#191919", "#666666", "#e5e5e5"
plt.rcParams.update({
    "font.size": 9, "axes.edgecolor": TEXT2, "axes.labelcolor": TEXT, "xtick.color": TEXT2, "ytick.color": TEXT2,
    "axes.spines.top": False, "axes.spines.right": False, "axes.grid": True, "grid.color": GRID,
    "grid.linewidth": 0.8, "axes.titleweight": "bold", "axes.titlesize": 10, "legend.frameon": False,
    "savefig.dpi": 200, "savefig.bbox": "tight",
})

# ---- reading ----
runs = defaultdict(list)          # (group, service, variant) -> [run], one per seed
for path in sorted(glob.glob(f"{RAW}/*.csv")):
    name = os.path.basename(path)[:-4]
    parts = name.split("__")
    if name.endswith((".series", ".latency")) or ".part" in name:
        continue
    if len(parts) == 3:                     # no variant (events: e_sat__events__s1)
        parts = [parts[0], parts[1], "", parts[2]]
    if name.endswith((".series", ".latency")) or ".part" in name or len(parts) != 4 or not parts[3].startswith("s"):
        continue
    res = {}
    with open(path) as f:
        for r in csv.reader(f):
            if len(r) >= 3:
                res[(r[0], r[1])] = r[2]
    runs[tuple(parts[:3])].append({"name": name, "seed": int(parts[3][1:]), "res": res})


def num(run, scope, key):
    try:
        return float(run["res"].get((scope, key), 0))
    except ValueError:
        return 0.0


def e4t(run):
    """the per-second lines of the E4 build: [{t, half_open, established, closing, expired, rx, miss, ...}]"""
    rows = []
    try:
        with open(f"{RAW}/{run['name']}.log") as f:
            for line in f:
                if line.startswith("E4T "):
                    rows.append({k: int(v) for k, v in (kv.split("=") for kv in line.split()[1:])})
    except OSError:
        pass
    return rows


def series(run, service):
    rows = []
    try:
        with open(f"{RAW}/{run['name']}.series.csv") as f:
            for r in csv.DictReader(f):
                if r["service"] == service:
                    rows.append({k: (float(v) if k != "service" else v) for k, v in r.items()})
    except OSError:
        pass
    return rows


def mmm(vals):
    vals = list(vals)
    return (statistics.median(vals), min(vals), max(vals)) if vals else None


def fmt(t, digits=0, scale=1.0):
    """median [min, max], or the median alone when the seeds agree"""
    if t is None:
        return "-"
    m, lo, hi = (x * scale for x in t)
    f = (lambda x: f"{x:,.{digits}f}")
    return f(m) if f(lo) == f(hi) else f"{f(m)} [{f(lo)}, {f(hi)}]"


def col(key_runs, fn):
    return mmm(fn(r) for r in key_runs) if key_runs else None


lines = []                         # summary.txt
tex = []                           # e4_tables.tex


def w(s=""):
    lines.append(s)
    print(s)


def tex_table(caption, label, header, rows, align):
    tex.append("\\begin{table}[H]\n\\centering\\small")
    tex.append(f"\\caption{{{caption}}}\n\\label{{{label}}}")
    tex.append(f"\\begin{{tabular}}{{{align}}}\n\\toprule")
    tex.append(" & ".join(header) + " \\\\\n\\midrule")
    for r in rows:
        tex.append(("\\midrule" if r == "MID" else " & ".join(r) + " \\\\"))
    tex.append("\\bottomrule\n\\end{tabular}\n\\end{table}\n")


def texnum(s):
    """median, with [min, max] in small type on a second line"""
    if "[" not in s:
        return s
    m, rest = s.split(" [", 1)
    return f"\\begin{{tabular}}[t]{{@{{}}r@{{}}}}{m}\\\\[-3pt]{{\\scriptsize[{rest}}}\\end{{tabular}}"


# ---- functional: the error table ----
w("E4 (each service alone). Medians over the seeds, [min, max] where they differ.")
w()
w("Error table, published scenarios (functional mode, our configuration):")
OUTCOMES = [("broken flows", "svc", "broken_flows"), ("lost flows", "svc", "lost_flows"),
            ("misplaced flows", "svc", "misplaced_flows"), ("malformed", "svc", "malformed"),
            ("misforwarded", "none", "misforwarded"), ("leaks", "run", "leaks"),
            ("drops (service)", "svc", "pkts_dropped"), ("failed inserts", "svc", "inserts_failed"),
            ("peak memory (MB)", "run", "peak_memory")]
trows = []
for svc in SERVICES + ["events"]:
    rs = runs.get(("f_out", svc, "published"), [])
    if not rs:
        continue
    scope = "web" if svc == "events" else svc
    vals = []
    for label, sc, key in OUTCOMES:
        s = {"svc": scope}.get(sc, sc)
        if key == "peak_memory":
            vals.append(fmt(col(rs, lambda r: num(r, "run", key) / 1e6), 2))
        else:
            vals.append(fmt(col(rs, lambda r, s=s, key=key: num(r, s, key))))
    w(f"  {svc + '.scn':12} " + "  ".join(f"{lab}={v}" for (lab, _, _), v in zip(OUTCOMES, vals)))
    trows.append([f"\\texttt{{{svc}.scn}}"] + [texnum(v) for v in vals])
if trows:
    head = ["Scenario", "Broken", "Lost", "Misplaced", "Malformed", "Misforwarded", "Leaks", "Drops",
            "Failed inserts", "Memory (MB)"]
    keep = {0, 1, 2, 3, len(head) - 1}            # always shown; the others only if not 0 in every run
    zero = [i for i in range(len(head)) if i not in keep and all(r[i] == "0" for r in trows)]
    cols = [i for i in range(len(head)) if i not in zero]
    names = [head[i].lower() for i in zero]
    note = (" " + (", ".join(names[:-1]) + " and " + names[-1] if len(names) > 1 else names[0]).capitalize()
            + " were 0 in every run.") if names else ""
    tex_table("Outcomes of the error table, each service alone on its published scenario (functional mode, our "
              "configuration; median [min, max] over 5 seeds)." + note, "tab:e4-outcomes",
              [head[i] for i in cols], [[r[i] for i in cols] for r in trows], "l" + "r" * (len(cols) - 1))
w()

# ---- chat churn ----
w("Chat churn (functional): replica churn alone = chat.scn; LB churn alone = no upgrade/failures/scale-out, an LB")
w("joins at 100 s (20 %) and one leaves at 540 s (25 %); both = chat.scn plus those two.")
crow = []
for label, key in [("replica churn alone", ("f_out", "chat", "published")), ("LB churn alone", ("f_churn", "chat", "lb")),
                   ("both", ("f_churn", "chat", "both"))]:
    rs = runs.get(key, [])
    if not rs:
        continue
    vals = [fmt(col(rs, lambda r, k=k: num(r, "chat", k))) for k in
            ("broken_flows", "lost_flows", "misplaced_flows", "flows_moved_in", "flows_moved_out")]
    w(f"  {label:20} broken={vals[0]}  lost={vals[1]}  misplaced={vals[2]}  moved in={vals[3]}  moved out={vals[4]}")
    crow.append([label] + [texnum(v) for v in vals])
if crow:
    tex_table("Chat under churn (functional mode, median [min, max] over 5 seeds).", "tab:e4-churn",
              ["Churn", "Broken", "Lost", "Misplaced", "Moved in", "Moved out"], crow, "lrrrrr")
# when do chat's broken flows happen (both): from the harness's series
for r in runs.get(("f_churn", "chat", "both"), [])[:1]:
    br = [(int(x["t_s"]), x["broken_flows"]) for x in series(r, "chat") if x["broken_flows"] > 0]
    if br:
        tot = sum(b for _, b in br)
        top = sorted(br, key=lambda x: -x[1])[:5]
        w(f"  both, seed {r['seed']}: {tot:.0f} broken flows; the seconds with most: " +
          ", ".join(f"t={t} s: {b:.0f}" for t, b in top))
w()


# ---- timeouts ----
def table_peaks(r):
    rows = e4t(r)
    if not rows:
        return None, None
    live = max(x["half_open"] + x["established"] + x["closing"] for x in rows)
    dead = max(x["expired"] for x in rows)
    return live, dead


w("Idle timeouts (functional): broken flows, replica computations (misses), and the table's peak entries: live")
w("(half-open + established + closing) and expired (not reclaimed yet).")
trows = []
for svc, variants in [("chat", [("est30000", "30 s"), ("est60000", "60 s"), ("published", "70 s (ours)"),
                                ("est120000", "120 s")]),
                      ("video", [("est2000", "2 s"), ("est5000", "5 s"), ("published", "70 s (ours)")]),
                      ("web", [("est5000", "est 5 s"), ("published", "est 30 s, fin 2 s (ours)"),
                               ("fin500", "fin 0.5 s")])]:
    for var, label in variants:
        rs = runs.get(("f_out" if var == "published" else "f_to", svc, var), [])
        if not rs:
            continue
        b = fmt(col(rs, lambda r: num(r, svc, "broken_flows")))
        rc = fmt(col(rs, lambda r: num(r, svc, "replica_computations")))
        peaks = [table_peaks(r) for r in rs]
        live = fmt(mmm(p[0] for p in peaks if p[0] is not None))
        dead = fmt(mmm(p[1] for p in peaks if p[1] is not None))
        w(f"  {svc:6} {label:26} broken={b}  replica computations={rc}  live entries (peak)={live}  expired (peak)={dead}")
        trows.append([svc, label, texnum(b), texnum(rc), texnum(live), texnum(dead)])
    trows.append("MID")
if trows:
    tex_table("Idle timeouts, each service alone on its published scenario (functional mode, median [min, max] over "
              "5 seeds). Live entries: half-open, established or closing; expired: past their timeout, kept until an "
              "insert reuses the slot.", "tab:e4-timeouts",
              ["Service", "Timeout", "Broken", "Replica comp.", "Live entries", "Expired"], trows[:-1], "llrrrr")
w()

# ---- table size and insert fallback ----
w("A table too small for the service, with each insert fallback (functional): 0 forward without an entry,")
w("1 evict the oldest entry of the key's buckets, 2 evict a half-open one, then a closing one, then the oldest,")
w("3 the oldest of the key's own service.")
frows = []
for svc, var, size in [("chat", "b40000", "320k slots"), ("web", "b4096", "32k slots"), ("video", "b256", "2k slots")]:
    for fb in "0123":
        rs = runs.get(("f_fb", svc, f"{var}f{fb}"), [])
        if not rs:
            continue
        b = fmt(col(rs, lambda r: num(r, svc, "broken_flows")))
        fail = fmt(col(rs, lambda r: num(r, svc, "inserts_failed")))
        ev = fmt(col(rs, lambda r: sum(x["evict"] for x in e4t(r))))
        rc = fmt(col(rs, lambda r: num(r, svc, "replica_computations")))
        mis = fmt(col(rs, lambda r: num(r, svc, "misplaced_flows")))
        w(f"  {svc:6} {size:11} fallback {fb}: broken={b}  failed inserts={fail}  evictions={ev}  replica "
          f"computations={rc}  misplaced={mis}")
        frows.append([svc, size, fb, texnum(b), texnum(fail), texnum(ev), texnum(rc)])
    frows.append("MID")
if frows:
    tex_table("A table too small for the service, with each insert fallback (functional mode, median [min, max] over "
              "5 seeds; video on the first 5 minutes of its scenario). Fallback 0: forward without an entry; 1: evict "
              "the entry of the key's buckets seen longest ago; 2: a half-open entry first, then a closing one, then "
              "the oldest; 3 (web's, ours): 1 limited to the key's own service.", "tab:e4-fallback",
              ["Service", "Table", "Fallback", "Broken", "Failed inserts", "Evictions", "Replica comp."],
              frows[:-1], "llcrrrr")
try:
    with open(f"{RAW}/web_flow_ends.csv") as f:
        ends = {r["end"]: int(r["flows"]) for r in csv.DictReader(f)}
    tot = sum(ends.values())
    w("How web flows end (first 100 s of web.scn, seed 1): " +
      ", ".join(f"{k} {100 * v / tot:.1f} %" for k, v in sorted(ends.items(), key=lambda kv: -kv[1])))
except OSError:
    pass
w()

# ---- saturation ----
w("Saturation mode (2-minute traces): packets per second, by batch size.")
srows = []
sat = {}
for svc in SERVICES:
    for b in BATCHES:
        rs = runs.get(("s_batch", svc, f"b{b}"), [])
        if not rs:
            continue
        t = col(rs, lambda r: num(r, "run", "raw_pps") / 1e6)
        valid = sum(num(r, "run", "speed_valid") == 1 for r in rs)
        sat[(svc, b)] = t
        w(f"  {svc:6} batch {b:2}: {fmt(t, 2)} Mpps  (speed_valid in {valid}/{len(rs)} runs)")
        srows.append([svc, str(b), texnum(fmt(t, 2))])
for svc, var, size in [("video", "b1024", "8k slots"), ("web", "b8192", "65k slots")]:
    rs = runs.get(("s_table", svc, var), [])
    if rs:
        t = col(rs, lambda r: num(r, "run", "raw_pps") / 1e6)
        mem = col(rs, lambda r: num(r, "run", "peak_memory") / 1e6)
        w(f"  {svc:6} batch 64, table of {size}: {fmt(t, 2)} Mpps, peak memory {fmt(mem, 1)} MB")
all_sat = [r for (g, _, _), rs in runs.items() if g in ("s_batch", "s_table", "e_sat") for r in rs]
if all_sat:
    idle = col(all_sat, lambda r: num(r, "run", "idle_fraction") * 100)
    gen = col(all_sat, lambda r: num(r, "all", "pkts_rx") / max(num(r, "run", "real_time_s"), 1e-9) / 1e6)
    w(f"  the loop idle (waiting for packets): {fmt(idle, 0)} % of the time; the generator delivers {fmt(gen, 2)} "
      f"Mpps; speed_valid in {sum(num(r, 'run', 'speed_valid') == 1 for r in all_sat)}/{len(all_sat)} runs")
# the table size: the autotuner's table (batch 64 runs) against a table sized for the service
trows = []
for svc, var, slots in [("video", "b1024", 1024 * 8), ("web", "b8192", 8192 * 8)]:
    big, small = runs.get(("s_batch", svc, "b64"), []), runs.get(("s_table", svc, var), [])
    if not (big and small):
        continue
    for rs, label in [(big, None), (small, f"{slots:,}")]:
        if label is None:      # the autotuner's slots: ft_buckets x bucket_size, from params.txt (run_e4.sh)
            try:
                with open(f"{OUT}/params.txt") as f:
                    p = dict(kv.split("=", 1) for kv in f.read().strip().split(",") if "=" in kv)
                label = f"{int(p['ft_buckets']) * int(p.get('bucket_size', 8)):,} (autotuner)"
            except (OSError, KeyError, ValueError):
                label = "autotuner"
        trows.append([svc, label, texnum(fmt(col(rs, lambda r: num(r, "run", "peak_memory") / 1e6), 1)),
                      texnum(fmt(col(rs, lambda r: num(r, "run", "raw_pps") / 1e6), 2))])
if trows:
    tex_table("Packet rate with the autotuner's table and with a table sized for the service (saturation mode, "
              "batch 64, 2-minute traces, median [min, max] over 5 seeds). No insert failed and no flow broke in "
              "these runs.", "tab:e4-tablesize", ["Service", "Table (slots)", "Peak memory (MB)", "Mpps"], trows,
              "llrr")
w()

# ---- events ----
w("Event cost (events.scn, saturation mode, profile=1): the time in on_event, and in hash (where ev_coalesce=1")
w("applies the recorded events, at the start of the next batch).")
events_rows = {}
for var, label in [("events", "with its events"), ("events_coalesce", "with them, coalesced"),
                   ("events_none", "without them")]:
    rs = runs.get(("e_sat", var, ""), [])
    if not rs:
        continue
    pps = col(rs, lambda r: num(r, "run", "raw_pps") / 1e6)
    calls = col(rs, lambda r: num(r, "on_event", "calls"))
    on_ev = col(rs, lambda r: num(r, "on_event", "time_ns") / 1e6)
    hsh = col(rs, lambda r: num(r, "hash", "time_ns") / 1e6)
    both = col(rs, lambda r: (num(r, "on_event", "time_ns") + num(r, "hash", "time_ns")) / 1e6)
    busy = col(rs, lambda r: num(r, "run", "lb_busy_s") * 1e3)
    per = col(rs, lambda r: num(r, "on_event", "time_ns") / max(num(r, "on_event", "calls"), 1) / 1e3)
    batch = col(rs, lambda r: num(r, "hash", "pkts") / max(num(r, "hash", "calls"), 1))
    events_rows[var] = (pps, calls, on_ev, hsh, both, per)
    w(f"  {label:22} {fmt(pps, 2)} Mpps; events {fmt(calls)}; on_event {fmt(on_ev, 1)} ms ({fmt(per, 1)} us each); "
      f"hash stage {fmt(hsh, 1)} ms; both {fmt(both, 1)} ms; loop busy {fmt(busy, 0)} ms; "
      f"{fmt(batch, 1)} packets per hash call")
if "events_none" in events_rows:
    base = events_rows["events_none"][4][0]
    for var, label in [("events", "with its events"), ("events_coalesce", "with them, coalesced")]:
        if var in events_rows:
            extra = events_rows[var][4][0] - base
            n = max(events_rows[var][1][0], 1)
            w(f"  {label}: on_event + hash {extra:.1f} ms above the run without events, {extra / n * 1e3:.1f} us "
              f"per event (medians)")
if events_rows:
    rows_t = []
    for var, label in [("events", "with its events"), ("events_coalesce", "same, coalesced"),
                       ("events_none", "without them")]:
        if var in events_rows:
            pps, calls, on_ev, hsh, both, per = events_rows[var]
            rows_t.append([label, texnum(fmt(on_ev, 1)), texnum(fmt(hsh, 1)), texnum(fmt(both, 1)),
                           texnum(fmt(pps, 2))])
    n_ev = events_rows.get("events", events_rows.get("events_coalesce"))
    n_ev = f"; the runs with events have {fmt(n_ev[1])} of them" if n_ev else ""
    tex_table("Event cost on \\texttt{events.scn} (saturation mode, \\texttt{profile=1}, median [min, max] over 5 "
              "seeds): time in \\texttt{on\\_event}, in the hash stage (where coalesced events are applied), and in "
              f"both{n_ev}.", "tab:e4-events",
              ["events.scn", "on\\_event (ms)", "hash (ms)", "Both (ms)", "Mpps"], rows_t, "lrrrr")
w()

# ---- paced ----
paced = {}
for (g, svc, var), rs in runs.items():
    if g == "p" and "l" in var:
        b, load = var[1:].split("l")
        paced[(svc, int(b), load)] = rs
if paced:
    # only the runs the harness validated (pace_valid = 1): in the others its generator fell behind the schedule
    w("Paced mode, valid runs only (pace_valid = 1): latency median / p99 (us) and RX drops (% of packets), at 50, 80, 95 %")
    w("of the saturation rate; n = valid runs of 5.")
    prows = []
    for svc in SERVICES:
        for b in BATCHES:
            cells, tcells, any_ok = [], [], False
            for load in LOADS:
                rs = paced.get((svc, b, load), [])
                ok = [r for r in rs if num(r, "run", "pace_valid") == 1]
                if not ok:
                    cells.append(f"no valid run of {len(rs)}" if rs else "-")
                    tcells.append("--")
                    continue
                any_ok = True
                med = col(ok, lambda r: num(r, "run", "latency_median_ns") / 1e3)
                p99 = col(ok, lambda r: num(r, "run", "latency_p99_ns") / 1e3)
                drop = col(ok, lambda r: 100 * num(r, "all", "pkts_ring_dropped") / max(num(r, "all", "pkts_rx"), 1))
                cells.append(f"{fmt(med, 1)} / {fmt(p99, 0)} us, drops {fmt(drop, 2)} % (n={len(ok)})")
                tcells.append(f"{med[0]:.1f} / {p99[0]:.0f} ({len(ok)})")
            w(f"  {svc:6} batch {b:2}: " + " | ".join(cells))
            if any_ok:
                prows.append([svc, str(b)] + tcells)
    for var in ("events", "events_coalesce", "events_none"):
        for load in LOADS:
            rs = [r for k, v in runs.items() if k[0] == "pe" and k[1] == var and k[2] == f"l{load}" for r in v]
            if rs:
                ok = [r for r in rs if num(r, "run", "pace_valid") == 1]
                if not ok:
                    w(f"  {var:16} load {load}: no valid run of {len(rs)}")
                    continue
                med = col(ok, lambda r: num(r, "run", "latency_median_ns") / 1e3)
                p99 = col(ok, lambda r: num(r, "run", "latency_p99_ns") / 1e3)
                w(f"  {var:16} load {load}: median {fmt(med, 1)} us, p99 {fmt(p99, 0)} us (n={len(ok)})")
    allp = [r for k, v in runs.items() if k[0] in ("p", "pe") for r in v]
    if allp:
        okp = [r for r in allp if num(r, "run", "pace_valid") == 1]
        lag = lambda rs: col(rs, lambda r: num(r, "run", "pace_lag_p99_ns") / 1e3)
        w(f"  pace_valid in {len(okp)}/{len(allp)} runs; generator's p99 lag behind the schedule: valid runs "
          f"{fmt(lag(okp), 0) if okp else '-'} us, the others {fmt(lag([r for r in allp if r not in okp]), 0)} us")
    if prows:
        tex_table("Paced mode, runs the harness validated only (\\texttt{pace\\_valid} = 1): median / p99 latency "
                  "in $\\mu$s, median over the valid runs, with their number out of 5 in parentheses. Chat and "
                  "\\texttt{events.scn} have no valid run.", "tab:e4-paced",
                  ["Service", "Batch", "50\\,\\%", "80\\,\\%", "95\\,\\%"], prows, "llccc")
    w()

try:
    with open(f"{OUT}/machine.txt") as m:
        w("Machine:")
        for line in m:
            w("  " + line.rstrip())
except OSError:
    pass

with open(f"{OUT}/summary.txt", "w") as f:
    f.write("\n".join(lines) + "\n")
with open(f"{OUT}/e4_tables.tex", "w") as f:
    f.write("\n".join(tex) + "\n")


# ---- figures ----
def log2_batch_axis(ax):
    ax.set_xscale("log", base=2)
    ax.xaxis.set_major_locator(FixedLocator(BATCHES))
    ax.xaxis.set_minor_locator(NullLocator())
    ax.xaxis.set_major_formatter(FuncFormatter(lambda x, _: f"{int(x)}"))
    ax.set_xlabel("batch size (packets per receive call)")


if sat:
    fig, ax = plt.subplots(figsize=(5.2, 3.3))
    for svc, color, marker in zip(SERVICES, (BLUE, ORANGE, AQUA), ("o", "s", "^")):
        xs = [b for b in BATCHES if (svc, b) in sat]
        if not xs:
            continue
        m = [sat[(svc, b)][0] for b in xs]
        lo = [sat[(svc, b)][0] - sat[(svc, b)][1] for b in xs]
        hi = [sat[(svc, b)][2] - sat[(svc, b)][0] for b in xs]
        ax.errorbar(xs, m, yerr=[lo, hi], color=color, marker=marker, markersize=6, linewidth=2, elinewidth=1,
                    capsize=2, label=svc, markeredgecolor="white")
    log2_batch_axis(ax)
    ax.set_ylabel("Mpps")
    ax.set_ylim(bottom=0)
    ax.set_title("saturation rate, each service alone")
    ax.legend(loc="lower right", fontsize=8.5)
    fig.tight_layout()
    fig.savefig(f"{OUT}/e4_saturation.png")
    plt.close(fig)

web1 = [r for r in runs.get(("f_out", "web", "published"), []) if r["seed"] == 1]
if web1:
    rows = e4t(web1[0])
    if rows:
        fig, ax = plt.subplots(figsize=(6.5, 3.3))
        t = [x["t"] for x in rows]
        for key, label, color, style in [("expired", "expired (flow over, slot not reused yet)", YELLOW, "-"),
                                         ("established", "established", ORANGE, "--"),
                                         ("closing", "closing (FIN or RST seen)", AQUA, "-."),
                                         ("half_open", "half-open", BLUE, ":")]:
            ax.plot(t, [max(x[key], 1) for x in rows], color=color, linestyle=style, linewidth=2, label=label)
        ax.set_yscale("log")
        ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y:,.0f}"))
        ax.set_xlabel("trace time (s)")
        ax.set_ylabel("entries")
        ax.set_title("web's table over time (web.scn, seed 1)")
        ax.set_ylim(0.8, 3e6)
        ax.legend(loc="lower right", fontsize=8, ncol=2)
        fig.tight_layout()
        fig.savefig(f"{OUT}/e4_web_table.png")
        plt.close(fig)

chat1 = [r for r in runs.get(("f_out", "chat", "published"), []) if r["seed"] == 1]
if chat1:
    rows = e4t(chat1[0])
    lost = {int(x["t_s"]): x["lost_flows"] for x in series(chat1[0], "chat")}
    if rows:
        fig, (a1, a2) = plt.subplots(2, 1, figsize=(6.5, 4.6), sharex=True)
        t = [x["t"] for x in rows]
        a1.plot(t, [x["syn"] for x in rows], color=BLUE, linewidth=1.5, label="new flows (SYN misses) per second")
        lt = [s for s, v in sorted(lost.items()) if v > 0]
        a1.plot(lt, [lost[s] for s in lt], linestyle="none", marker="v", markersize=5, color=ORANGE,
                label="flows lost in that second (replica down)")
        a1.set_yscale("log")
        a1.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y:,.0f}"))
        a1.set_ylabel("per second")
        a1.set_ylim(50, 3e6)
        a1.set_title("chat over time (chat.scn, seed 1)")
        a1.legend(loc="upper left", fontsize=8, ncol=2)
        a2.plot(t, [x["established"] for x in rows], color=ORANGE, linewidth=2, linestyle="--", label="established")
        a2.plot(t, [x["expired"] for x in rows], color=YELLOW, linewidth=2, label="expired")
        a2.plot(t, [x["half_open"] + x["closing"] for x in rows], color=AQUA, linewidth=2, linestyle="-.",
                label="half-open or closing")
        a2.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y / 1000:,.0f}k"))
        a2.set_ylabel("entries")
        a2.set_xlabel("trace time (s)")
        a2.legend(loc="lower right", bbox_to_anchor=(1.0, 0.08), fontsize=8)
        fig.tight_layout()
        fig.savefig(f"{OUT}/e4_chat_storm.png")
        plt.close(fig)

if paced:
    fig, axes = plt.subplots(2, 3, figsize=(10, 5.6), sharex=True)
    for j, svc in enumerate(SERVICES):
        for b, color, marker in zip(BATCHES, (BLUE, ORANGE, AQUA, YELLOW), ("o", "s", "^", "D")):
            for i, key in enumerate(("latency_median_ns", "latency_p99_ns")):
                xs, ys = [], []
                for load in LOADS:
                    rs = [r for r in paced.get((svc, b, load), []) if num(r, "run", "pace_valid") == 1]
                    if rs:
                        xs.append(float(load))
                        ys.append(statistics.median(num(r, "run", key) / 1e3 for r in rs))
                if xs:
                    axes[i][j].plot(xs, ys, color=color, marker=marker, markersize=5, linewidth=1.8,
                                    label=f"batch {b}", markeredgecolor="white")
        axes[0][j].set_title(svc)
        for i in range(2):
            axes[i][j].set_yscale("log")
            axes[i][j].yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y:g}"))
            axes[i][j].xaxis.set_major_locator(FixedLocator([float(x) for x in LOADS]))
        axes[1][j].set_xlabel("load (share of the saturation rate)")
    axes[0][0].set_ylabel("median latency (\u00b5s)")
    axes[1][0].set_ylabel("p99 latency (\u00b5s)")
    axes[0][0].legend(loc="upper left", fontsize=8)
    fig.tight_layout()
    fig.savefig(f"{OUT}/e4_paced.png")
    plt.close(fig)

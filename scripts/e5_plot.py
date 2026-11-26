#!/usr/bin/env python3
"""E5: figures and summary from the CSV files of scripts/e5_shared.sh.

    python3 scripts/e5_plot.py [build/e5]

Reads func.csv, occ_*.csv, stress.csv, sat.csv, paced.csv and machine.txt in that folder (whichever exist),
writes e5_occ.png, e5_series.png, e5_stress.png, e5_sat.png and summary.md there, and prints the summary.
Every value is the median over the seeds, with the min-max spread, as the brief asks.

Author: Ludovic
"""
import csv
import glob
import os
import statistics
import sys
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")                   # write PNG files, open no window
import matplotlib.pyplot as plt  # noqa: E402

D = sys.argv[1] if len(sys.argv) > 1 else "build/e5"
SERVICES = ["chat", "video", "web"]
VARIANTS = ["noflood", "noflood_guard", "flood", "flood_stateless", "flood_guard"]
LABEL = {"noflood": "mixed, no defence", "noflood_guard": "mixed, syn_guard", "flood": "flood, no defence",
         "flood_stateless": "flood, syn_insert=0", "flood_guard": "flood, syn_guard"}
COLOR = {"noflood": "#8c8c8c", "noflood_guard": "#bdbdbd", "flood": "#f47230", "flood_stateless": "#9b59b6",
         "flood_guard": "#257de0"}
SVC_COLOR = {"chat": "#257de0", "video": "#2ca58d", "web": "#f47230", "web_half": "#f9b384", "expired": "#d9d9d9"}
GREY = "#666666"
FLOOD = (300, 330)

plt.rcParams.update({
    "font.size": 9, "axes.spines.top": False, "axes.spines.right": False, "axes.edgecolor": "#bbbbbb",
    "axes.grid": True, "grid.color": "#e6e6e6", "grid.linewidth": 0.6, "axes.axisbelow": True,
    "xtick.color": GREY, "ytick.color": GREY, "axes.labelcolor": "#191919", "legend.frameon": False,
    "axes.titlesize": 9, "axes.titlelocation": "left", "figure.dpi": 150, "savefig.bbox": "tight",
})

summary = []


def read(name):
    """the rows of a CSV file of the folder, as dicts; None if the file is missing"""
    path = os.path.join(D, name)
    if not os.path.exists(path):
        return None
    with open(path) as f:
        return list(csv.DictReader(f))


def num(x):
    try:
        return float(x)
    except ValueError:
        return float("nan")


def med(values):
    return statistics.median(values), min(values), max(values)


def cell(values, digits=0):
    """'median [min-max]' for a table"""
    if not values:
        return "-"
    m, lo, hi = med(values)
    if lo == hi:
        return f"{m:,.{digits}f}"
    return f"{m:,.{digits}f} [{lo:,.{digits}f} - {hi:,.{digits}f}]"


def results(rows, labels):
    """rows of a collected CSV -> {labels tuple: {(scope, key): value}}"""
    out = defaultdict(dict)
    for r in rows:
        out[tuple(r[k] for k in labels)][(r["scope"], r["key"])] = num(r["value"])
    return out


def per_variant(res, scope, key, variants):
    """{variant: [value per seed]}"""
    out = defaultdict(list)
    for (v, _seed), d in res.items():
        if (scope, key) in d:
            out[v].append(d[(scope, key)])
    return {v: out[v] for v in variants if v in out}


machine = os.path.join(D, "machine.txt")
if os.path.exists(machine):
    summary += ["## Machine", "", "```", open(machine).read().strip(), "```", ""]

# ---- func: the outcomes per service and variant ----
func = read("func.csv")
if func:
    res = results(func, ["variant", "seed"])
    variants = [v for v in VARIANTS if any(k[0] == v for k in res)]
    nseeds = len({k[1] for k in res})
    summary += [f"## Functional mode: outcomes per service (median [min - max] over {nseeds} seeds)", ""]
    keys = ["broken_flows", "lost_flows", "misplaced_flows", "inserts_failed", "index_misses", "inserts_stored",
            "effective_pkts"]
    for svc in SERVICES:
        summary += [f"### {svc}", "", "| variant | " + " | ".join(keys) + " |", "|---" * (len(keys) + 1) + "|"]
        for v in variants:
            cells = [cell(per_variant(res, svc, k, [v]).get(v, [])) for k in keys]
            summary.append(f"| {LABEL[v]} | " + " | ".join(cells) + " |")
        summary.append("")
    summary += ["### whole run", "", "| variant | peak_memory (B) | leaks | misforwarded | malformed | flood_fwd |",
                "|---|---|---|---|---|---|"]
    for v in variants:
        g = lambda s, k: cell(per_variant(res, s, k, [v]).get(v, []))  # noqa: E731
        summary.append(f"| {LABEL[v]} | {g('run', 'peak_memory')} | {g('run', 'leaks')} | "
                       f"{g('run', 'misforwarded')} | {g('all', 'malformed')} | {g('all', 'flood_fwd')} |")
    summary.append("")

    # e5_series: broken flows and failed inserts per 10 s, chat + video, seed 1
    fig, axes = plt.subplots(2, 1, figsize=(7.5, 4.6), sharex=True)
    for v in variants:
        path = os.path.join(D, "runs", f"func_{v}_1.csv.series.csv")
        if not os.path.exists(path):
            continue
        bins = defaultdict(lambda: [0, 0])
        with open(path) as f:
            for r in csv.DictReader(f):
                if r["service"] in ("chat", "video"):
                    b = int(float(r["t_s"]) // 10) * 10
                    bins[b][0] += num(r["broken_flows"])
                    bins[b][1] += num(r["inserts_failed"])
        ts = sorted(bins)
        axes[0].step(ts, [bins[t][0] for t in ts], where="post", color=COLOR[v], label=LABEL[v], lw=1.2)
        axes[1].step(ts, [bins[t][1] for t in ts], where="post", color=COLOR[v], label=LABEL[v], lw=1.2)
    for ax in axes:
        ax.axvspan(*FLOOD, color="#f47230", alpha=0.08, lw=0)
        for t in (100, 540):
            ax.axvline(t, color=GREY, lw=0.6, ls=":")
    axes[0].set_ylabel("broken flows / 10 s")
    axes[1].set_ylabel("failed inserts / 10 s")
    axes[1].set_xlabel("trace time (s)   ·   shaded: SYN flood on web   ·   dotted: LB join (100 s), leave (540 s)")
    axes[0].set_title("chat + video, seed 1")
    axes[0].legend(fontsize=7, ncol=3)
    fig.savefig(os.path.join(D, "e5_series.png"))
    plt.close(fig)

# ---- occ: occupancy of the shared table over time ----
occ_files = sorted(glob.glob(os.path.join(D, "occ_*.csv")))
if occ_files:
    occ = {}
    for path in occ_files:
        v = os.path.basename(path)[4:-4]
        rows = list(csv.DictReader(open(path)))
        if rows:
            occ[v] = rows
    order = [v for v in VARIANTS if v in occ]
    fig, axes = plt.subplots(1, len(order), figsize=(3.4 * len(order), 3.0), sharey=True, squeeze=False)
    summary += ["## Occupancy of the shared table (seed 1, sampled every second)", "",
                "| variant | capacity | peak live | peak live chat | peak live video | peak live web (half-open) | "
                "min free + expired |", "|---|---|---|---|---|---|---|"]
    for ax, v in zip(axes[0], order):
        by_t = defaultdict(dict)
        cap = 0
        for r in occ[v]:
            t, k = int(float(r["t_s"])), int(r["service"])
            by_t[t][k] = r
            cap = int(r["capacity"])
        ts = sorted(t for t in by_t if len(by_t[t]) == 3)
        get = lambda k, col: [num(by_t[t][k][col]) for t in ts]  # noqa: E731
        chat = [a + b + c for a, b, c in zip(get(0, "half_open"), get(0, "established"), get(0, "closing"))]
        video = [a + b + c for a, b, c in zip(get(1, "half_open"), get(1, "established"), get(1, "closing"))]
        web_half = get(2, "half_open")
        web_rest = [b + c for b, c in zip(get(2, "established"), get(2, "closing"))]
        expired = get(0, "expired")
        ax.stackplot(ts, chat, video, web_rest, web_half, expired,
                     colors=[SVC_COLOR["chat"], SVC_COLOR["video"], SVC_COLOR["web"], SVC_COLOR["web_half"],
                             SVC_COLOR["expired"]],
                     labels=["chat", "video", "web (est., closing)", "web half-open", "expired, not reclaimed"])
        ax.axhline(cap, color="#191919", lw=0.8, ls="--")
        ax.axvspan(*FLOOD, color="#f47230", alpha=0.08, lw=0)
        ax.set_title(LABEL[v])
        ax.set_xlabel("trace time (s)")
        live = [a + b + c + d for a, b, c, d in zip(chat, video, web_rest, web_half)]
        free = [cap - lv - e for lv, e in zip(live, expired)]
        summary.append(f"| {LABEL[v]} | {cap:,} | {max(live):,.0f} ({max(live) / cap:.1%}) | {max(chat):,.0f} | "
                       f"{max(video):,.0f} | {max(w + h for w, h in zip(web_rest, web_half)):,.0f} "
                       f"({max(web_half):,.0f}) | {min(f + e for f, e in zip(free, expired)):,.0f} |")
    axes[0][0].set_ylabel("index slots (dashed: capacity)")
    axes[0][0].legend(fontsize=6, loc="lower left")
    fig.savefig(os.path.join(D, "e5_occ.png"))
    plt.close(fig)
    summary.append("")

# ---- stress: web half-open timeout 1 s, so that the flood overflows the table ----
stress = read("stress.csv")
if stress:
    res = results(stress, ["variant", "seed"])
    variants = [v for v in ["flood", "flood_guard"] if any(k[0] == v for k in res)]
    keys = ["broken_flows", "inserts_failed", "index_misses", "effective_pkts"]
    summary += ["## Stress: the flood with web to_half_ms=1000 (median [min - max] over the seeds)", "",
                "| variant | service | " + " | ".join(keys) + " |", "|---|---" + "|---" * len(keys) + "|"]
    for v in variants:
        for svc in SERVICES + ["all"]:
            cells = [cell(per_variant(res, svc, k, [v]).get(v, [])) for k in keys]
            summary.append(f"| {LABEL[v]} | {svc} | " + " | ".join(cells) + " |")
    summary.append("")

    # chat + video failed inserts and broken flows per 10 s, seed 1
    fig, axes = plt.subplots(2, 1, figsize=(7.5, 4.6), sharex=True)
    for v in variants:
        path = os.path.join(D, "runs", f"stress_{v}_1.csv.series.csv")
        if not os.path.exists(path):
            continue
        bins = defaultdict(lambda: [0, 0])
        with open(path) as f:
            for r in csv.DictReader(f):
                if r["service"] in ("chat", "video"):
                    t = int(float(r["t_s"]) // 10) * 10
                    bins[t][0] += num(r["broken_flows"])
                    bins[t][1] += num(r["inserts_failed"])
        ts = sorted(bins)
        axes[0].step(ts, [bins[t][0] for t in ts], where="post", color=COLOR[v], label=LABEL[v], lw=1.2)
        axes[1].step(ts, [bins[t][1] for t in ts], where="post", color=COLOR[v], label=LABEL[v], lw=1.2)
    for ax in axes:
        ax.axvspan(*FLOOD, color="#f47230", alpha=0.08, lw=0)
    axes[0].set_ylabel("broken flows / 10 s")
    axes[1].set_ylabel("failed inserts / 10 s")
    axes[1].set_xlabel("trace time (s)   ·   shaded: SYN flood on web")
    axes[0].set_title("stress (web to_half_ms=1000): chat + video, seed 1")
    axes[0].legend(fontsize=7)
    fig.savefig(os.path.join(D, "e5_stress.png"))
    plt.close(fig)

# ---- sat: raw and effective rate ----
sat = read("sat.csv")
if sat:
    res = results(sat, ["variant", "seed"])
    variants = [v for v in VARIANTS if any(k[0] == v for k in res)]
    summary += ["## Saturation mode (pinned; median [min - max] over the seeds)", "",
                "| variant | raw Mpps | effective Mpps | useful fraction | speed_valid (all seeds) | stalls |",
                "|---|---|---|---|---|---|"]
    for v in variants:
        raw = [x / 1e6 for x in per_variant(res, "run", "raw_pps", [v]).get(v, [])]
        eff = [x / 1e6 for x in per_variant(res, "run", "effective_pps", [v]).get(v, [])]
        useful = per_variant(res, "run", "useful_fraction", [v]).get(v, [])
        valid = per_variant(res, "run", "speed_valid", [v]).get(v, [])
        stalls = per_variant(res, "run", "generator_stalls", [v]).get(v, [])
        summary.append(f"| {LABEL[v]} | {cell(raw, 3)} | {cell(eff, 3)} | {cell(useful, 4)} | "
                       f"{'yes' if valid and all(x == 1 for x in valid) else 'NO'} | {cell(stalls)} |")
    summary.append("")
    fig, ax = plt.subplots(figsize=(6, 2.8))
    w = 0.38
    for i, v in enumerate(variants):
        for j, (key, alpha) in enumerate([("raw_pps", 0.45), ("effective_pps", 1.0)]):
            x = [y / 1e6 for y in per_variant(res, "run", key, [v]).get(v, [])]
            if not x:
                continue
            m, lo, hi = med(x)
            ax.bar(i + (j - 0.5) * w, m, w, color=COLOR[v], alpha=alpha, yerr=[[m - lo], [hi - m]], capsize=2,
                   ecolor=GREY)
            ax.text(i + (j - 0.5) * w, m, f"{m:.2f}", ha="center", va="bottom", fontsize=7)
    ax.set_xticks(range(len(variants)), [LABEL[v] for v in variants], fontsize=7)
    ax.set_ylabel("Mpps (light: raw, dark: effective)")
    fig.savefig(os.path.join(D, "e5_sat.png"))
    plt.close(fig)

# ---- paced: the final configuration's latency ----
paced = read("paced.csv")
if paced:
    res = results(paced, ["variant", "seed"])
    pv = next(iter(res))[0]
    g = lambda k: per_variant(res, "run", k, [pv]).get(pv, [])  # noqa: E731
    us = lambda xs: [x / 1e3 for x in xs]  # noqa: E731
    summary += [f"## Paced mode: {LABEL[pv]} at 80 % of its saturation rate (median [min - max] over the seeds)", "",
                "| latency p50 (us) | p99 (us) | p99.9 (us) | ring drops | pace_valid (all seeds) |",
                "|---|---|---|---|---|",
                f"| {cell(us(g('latency_median_ns')), 2)} | {cell(us(g('latency_p99_ns')), 2)} | "
                f"{cell(us(g('latency_p999_ns')), 2)} | "
                f"{cell(per_variant(res, 'all', 'pkts_ring_dropped', [pv]).get(pv, []))} | "
                f"{'yes' if g('pace_valid') and all(x == 1 for x in g('pace_valid')) else 'NO'} |", ""]

text = "# E5: the three services together\n\n" + "\n".join(summary)
with open(os.path.join(D, "summary.md"), "w") as f:
    f.write(text)
print(text)

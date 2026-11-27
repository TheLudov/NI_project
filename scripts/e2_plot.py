#!/usr/bin/env python3
"""E2: figures and summary from the CSV files of scripts/e2_hash.sh.

    python3 scripts/e2_plot.py [build/e2]

Reads speed.csv, spread.csv, fill.csv, bench.csv, seed.txt and machine.txt in that folder (whichever exist), writes
e2_speed.png, e2_spread.png, e2_fill.png, e2_bench.png and summary.md there, and prints the summary.
Every value is the median over the 5 seeds (or repetitions), with the min-max spread, as the brief asks.

Author: Ludovic
"""
import csv
import os
import statistics
import sys

import matplotlib

matplotlib.use("Agg")                   # write PNG files, open no window
import matplotlib.pyplot as plt  # noqa: E402

D = sys.argv[1] if len(sys.argv) > 1 else "build/e2"
HFS = ["tab", "crc"]
COLOR = {"tab": "#257de0", "crc": "#f47230"}
LABEL = {"tab": "simple tabulation", "crc": "CRC32C"}
GREY = "#666666"

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


def med(values):
    """median, min, max"""
    return statistics.median(values), min(values), max(values)


def cell(values, digits=2):
    """'median [min-max]' for a table"""
    m, lo, hi = med(values)
    return f"{m:.{digits}f} [{lo:.{digits}f} - {hi:.{digits}f}]"


def bars(ax, groups, values, ylabel, digits=2):
    """Grouped bars from 0: one group per x position, one bar per hash function, its median with a min-max error bar,
    the median written above. values[(group, hf)] = the values over the seeds."""
    width = 0.38
    for j, hf in enumerate(HFS):
        xs = [i + (j - 0.5) * width for i in range(len(groups))]
        st = [med(values[(g, hf)]) for g in groups]
        ax.bar(xs, [s[0] for s in st], width, color=COLOR[hf], label=LABEL[hf], edgecolor="white", linewidth=2)
        ax.errorbar(xs, [s[0] for s in st], yerr=[[s[0] - s[1] for s in st], [s[2] - s[0] for s in st]],
                    fmt="none", ecolor="#191919", elinewidth=0.8, capsize=2)
        for x, s in zip(xs, st):
            ax.annotate(f"{s[0]:.{digits}f}", (x, s[2]), xytext=(0, 3), textcoords="offset points", ha="center",
                        fontsize=7, color=GREY)
    ax.set_xticks(range(len(groups)), groups)
    ax.set_ylabel(ylabel)
    ax.grid(axis="x", visible=False)


def dots(ax, groups, values, ylabel):
    """Like bars, for values that do not start from 0: the median as a dot, the min-max as a line."""
    for j, hf in enumerate(HFS):
        for i, g in enumerate(groups):
            m, lo, hi = med(values[(g, hf)])
            x = i + (j - 0.5) * 0.3
            ax.plot([x, x], [lo, hi], color=COLOR[hf], linewidth=2)
            ax.plot(x, m, "o", color=COLOR[hf], markersize=7, markeredgecolor="white", markeredgewidth=1.5,
                    label=LABEL[hf] if i == 0 else None)
    ax.set_xticks(range(len(groups)), groups)
    ax.set_ylabel(ylabel)
    ax.grid(axis="x", visible=False)


def save(fig, name):
    fig.savefig(os.path.join(D, name))
    plt.close(fig)


# ---- speed: ns per hash, minus the loop's own cost ("none", same repetition) ----
rows = read("speed.csv")
if rows:
    loop = {r["rep"]: float(r["ns_per_hash"]) for r in rows if r["hashfn"] == "none"}
    tests = {"hash": "hash alone", "locate": "hash + both buckets\n+ signature (ft_locate)"}
    net = {(tests[t], hf): [] for t in tests for hf in HFS}
    for r in rows:
        if r["hashfn"] in HFS:
            net[(tests[r["test"]], r["hashfn"])].append(float(r["ns_per_hash"]) - loop[r["rep"]])

    fig, ax = plt.subplots(figsize=(5, 3.2))
    bars(ax, list(tests.values()), net, "ns per hash (the loop's cost subtracted)")
    ax.legend(loc="upper left")
    ax.set_title("Time per hash, keys and tables in cache")
    save(fig, "e2_speed.png")

    summary.append("## Time per hash (ns, the loop's cost subtracted; median [min - max] over 5 repetitions)\n")
    summary.append("| test | tab | crc | tab - crc |\n|---|---|---|---|")
    for t, name in tests.items():
        v = {hf: net[(name, hf)] for hf in HFS}
        diff = statistics.median(v["tab"]) - statistics.median(v["crc"])
        summary.append(f"| {t} | {cell(v['tab'])} | {cell(v['crc'])} | {diff:+.2f} |")
    summary.append(f"\n(the loop alone: {cell(list(loop.values()))} ns per key)\n")

# ---- perf: hardware counters per hash, minus "none" (the loop and the setup), same repetition ----
rows = read("perf.csv")
if rows:
    events = ["instructions", "cycles", "L1-dcache-load-misses", "branch-misses"]
    count = {(r["hashfn"], r["rep"], r["event"]): int(r["count"]) / int(r["hashes"]) for r in rows}
    reps = sorted({r["rep"] for r in rows})
    summary.append("## perf stat, per hash (the loop's own counts subtracted; median [min - max] over 5 runs)\n")
    summary.append("| hash | " + " | ".join(events) + " | IPC |\n|---|" + "---|" * (len(events) + 1))
    for hf in HFS:
        per = {e: [count[(hf, r, e)] - count[("none", r, e)] for r in reps] for e in events}
        ipc = [i / c for i, c in zip(per["instructions"], per["cycles"])]
        summary.append(f"| {hf} | " + " | ".join(cell(per[e], 3) for e in events) + f" | {cell(ipc)} |")
    if os.path.exists(os.path.join(D, "asm.txt")):
        with open(os.path.join(D, "asm.txt")) as f:
            summary.append("\nThe compiled functions (gcc -O2 -S, asm.txt):\n\n```\n" + f.read() + "```")
    summary.append("")

# ---- spread: chi-square z and fullest bucket, per key set ----
rows = read("spread.csv")
if rows:
    dists = list(dict.fromkeys(r["dist"] for r in rows))
    bs = int(rows[0]["bucket_size"])
    z = {(d, hf): [float(r["z"]) for r in rows if r["dist"] == d and r["hashfn"] == hf] for d in dists for hf in HFS}
    full = {(d, hf): [int(r["fullest"]) for r in rows if r["dist"] == d and r["hashfn"] == hf]
            for d in dists for hf in HFS}

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(7.4, 3))
    ax1.axhspan(-2, 2, color="#eeeeee", linewidth=0, zorder=0)
    for j, hf in enumerate(HFS):
        for i, d in enumerate(dists):
            ax1.scatter([i + (j - 0.5) * 0.3] * len(z[(d, hf)]), z[(d, hf)], s=22, color=COLOR[hf],
                        edgecolor="white", linewidth=0.8, label=LABEL[hf] if i == 0 else None, zorder=3)
    lo, hi = min(min(v) for v in z.values()), max(max(v) for v in z.values())
    ax1.set_yscale("symlog", linthresh=10)          # linear from -10 to 10, logarithmic beyond: z can reach -100s
    ticks = [t for t in (-1000, -100, -10, -2, 0, 2, 10, 100, 1000) if min(lo, -3) * 1.5 <= t <= max(hi, 3) * 1.5]
    ax1.set_yticks(ticks, [str(t) for t in ticks])
    ax1.set_xticks(range(len(dists)), dists)
    ax1.grid(axis="x", visible=False)
    ax1.set_ylabel("chi-square z (one dot per seed)")
    ax1.set_title("Spread over the first buckets\n(grey: as random, |z| < 2; below: too even)")
    ax1.legend(loc="lower left", fontsize=7)

    bars(ax2, dists, full, "keys in the fullest first bucket", digits=0)
    ax2.axhline(bs, color="#999999", linestyle="--", linewidth=1, zorder=0)
    ax2.set_title(f"Fullest bucket ({rows[0]['keys']} keys, {rows[0]['buckets']} buckets)\n"
                  f"(dashed: the bucket size, {bs}, also the average)")
    save(fig, "e2_spread.png")

    summary.append(f"## Spread: {rows[0]['keys']} keys into {rows[0]['buckets']} first buckets, {bs} keys each on "
                   "average (median [min - max] over 5 seeds)\n")
    summary.append("| keys | hash | chi-square z (random: within +-2) | fullest bucket |\n|---|---|---|---|")
    for d in dists:
        for hf in HFS:
            summary.append(f"| {d} | {hf} | {cell(z[(d, hf)])} | {cell(full[(d, hf)], 0)} |")
    summary.append("")

# ---- fill: load factor at the first failed insert ----
rows = read("fill.csv")
if rows:
    r0 = rows[0]
    layout = (f"{r0['buckets']} buckets x {r0['bucket_size']} slots, max_depth {r0['max_depth']}, "
              f"dfs_budget {r0['dfs_budget']}")
    dists = list(dict.fromkeys(r["dist"] for r in rows))
    load = {(d, hf): [float(r["load_at_failure"]) for r in rows if r["dist"] == d and r["hashfn"] == hf]
            for d in dists for hf in HFS}

    fig, ax = plt.subplots(figsize=(5, 3))
    dots(ax, dists, load, "load factor at the first failed insert")
    ax.legend(loc="upper left", fontsize=7)
    ax.set_title(f"Effect on E1's load factor\n({layout})")
    save(fig, "e2_fill.png")

    summary.append(f"## Load factor at the first failed insert ({layout}; median [min - max] over 5 seeds)\n")
    summary.append("| keys | tab | crc |\n|---|---|---|")
    for d in dists:
        summary.append(f"| {d} | {cell(load[(d, 'tab')], 4)} | {cell(load[(d, 'crc')], 4)} |")
    summary.append("")

# ---- bench: the LB in the harness's bench mode ----
rows = read("bench.csv")
if rows:
    dists = list(dict.fromkeys(r["dist"] for r in rows))
    short = {d: d.replace("sequential_ports", "seq_ports") for d in dists}
    stages = [("hash_ns", "hash"), ("lookup_ns", "lookup"), ("insert_ns", "insert"), ("total_ns", "total")]
    ns = {(k, short[d], hf): [float(r[k]) for r in rows if r["dist"] == d and r["hashfn"] == hf]
          for k, _ in stages for d in dists for hf in HFS}

    fig, axs = plt.subplots(1, 2, figsize=(7.4, 3))
    for ax, (k, name) in zip(axs, [("hash_ns", "hash stage"), ("total_ns", "all four stages")]):
        bars(ax, list(short.values()), {(d, hf): ns[(k, d, hf)] for d in short.values() for hf in HFS},
             "ns per packet", digits=1)
        ax.set_title(f"The LB in bench mode: {name}")
    handles, labels = axs[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper right", ncol=2, bbox_to_anchor=(1.0, 1.08))
    save(fig, "e2_bench.png")

    summary.append("## The LB in the harness's bench mode, ns per packet (median [min - max] over 5 seeds)\n")
    summary.append("| keys | stage | tab | crc | tab - crc |\n|---|---|---|---|---|")
    for d in short.values():
        for k, name in stages:
            v = {hf: ns[(k, d, hf)] for hf in HFS}
            diff = statistics.median(v["tab"]) - statistics.median(v["crc"])
            summary.append(f"| {d} | {name} | {cell(v['tab'])} | {cell(v['crc'])} | {diff:+.2f} |")
    summary.append("")

# ---- the cache cost of the tables: computed ----
summary.append("""## Cache cost (computed)

| what | bytes | 64-byte cache lines | lines read per packet |
|---|---|---|---|
| tabulation tables, 13 x 256 x 8 B | 26,624 | 416: 81 % of a 32 KiB L1d, 54 % of a 48 KiB one | 13, random |
| CRC32C | 0 | 0 | 0 (CRC instructions only) |
| packet headers (Ethernet + IPv4 + TCP, 54 B) | 54 | 1-2 | 1-2 |
| a bucket of the index (bucket_size x 4 B) | 32-64 | 1-2 | 1-2 (2 buckets on a miss) |
| an entry | 32 | half a line | 1, on a signature match |
""")

if os.path.exists(os.path.join(D, "seed.txt")):
    with open(os.path.join(D, "seed.txt")) as f:
        summary.append("## Do collisions survive a change of seed? (seedcheck.c: 2M random keys, 32 low bits)\n")
        summary.append("```\n" + f.read() + "```\n")

head = "# E2 summary\n\n"
if os.path.exists(os.path.join(D, "machine.txt")):
    with open(os.path.join(D, "machine.txt")) as f:
        head += "```\n" + f.read() + "```\n\n"
text = head + "\n".join(summary)
with open(os.path.join(D, "summary.md"), "w") as f:
    f.write(text)
print(text)
print(f"figures: {D}/e2_*.png", file=sys.stderr)

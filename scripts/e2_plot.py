#!/usr/bin/env python3
"""E2: figures and summary from scripts/e2_hash.sh's CSV files.

    python3 scripts/e2_plot.py [build/e2]

Writes DIR/e2_*.png and DIR/summary.md (the medians over seeds or repetitions, with min-max), and prints the summary.
Each figure shows the median and the spread (min-max) over 5 seeds or repetitions, as the brief asks.

Author: Ludovic.
"""
import csv
import os
import statistics as st
import sys
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

D = sys.argv[1] if len(sys.argv) > 1 else "build/e2"
COL = {"tab": "#257de0", "crc": "#f47230"}          # categorical slots 1 and 2 (validated pair)
LBL = {"tab": "simple tabulation", "crc": "CRC32C"}
HFS = ["tab", "crc"]
TXT2 = "#666666"

plt.rcParams.update({
    "font.size": 9, "axes.spines.top": False, "axes.spines.right": False,
    "axes.edgecolor": "#bbbbbb", "axes.grid": True, "grid.color": "#e6e6e6", "grid.linewidth": 0.6,
    "axes.axisbelow": True, "xtick.color": TXT2, "ytick.color": TXT2, "axes.labelcolor": "#191919",
    "legend.frameon": False, "figure.dpi": 150, "savefig.bbox": "tight",
})

summary = []


def rows(name):
    p = os.path.join(D, name)
    if not os.path.exists(p):
        return None
    with open(p) as f:
        return list(csv.DictReader(f))


def med_rng(v):
    return st.median(v), min(v), max(v)


def bars(ax, groups, data, ylabel, fmt="{:.2f}", legend="best"):
    """grouped bars: groups on x, one bar per hash function; data[(group, hf)] = list of values"""
    w = 0.38
    for j, hf in enumerate(HFS):
        xs, ms, lo, hi = [], [], [], []
        for i, g in enumerate(groups):
            v = data.get((g, hf))
            if not v:
                continue
            m, a, b = med_rng(v)
            xs.append(i + (j - 0.5) * w)
            ms.append(m); lo.append(m - a); hi.append(b - m)
        ax.bar(xs, ms, w * 0.92, color=COL[hf], label=LBL[hf], edgecolor="white", linewidth=1)
        ax.errorbar(xs, ms, yerr=[lo, hi], fmt="none", ecolor="#191919", elinewidth=0.8, capsize=2)
        for x, m in zip(xs, ms) if fmt else ():
            ax.annotate(fmt.format(m), (x, m), xytext=(0, 3), textcoords="offset points", ha="center",
                        fontsize=7, color=TXT2)
    ax.set_xticks(range(len(groups)))
    ax.set_xticklabels(groups)
    ax.set_ylabel(ylabel)
    ax.grid(axis="x", visible=False)
    if legend:
        ax.legend(loc=legend)


# ---- speed: net ns per hash (the hash's time minus the loop's own, "none", same rep, same K) ----
sp = rows("speed.csv")
if sp:
    raw = defaultdict(dict)   # (test, K, kb, rep) -> hf -> ns
    for r in sp:
        raw[(r["test"], int(r["K"]), int(r["pollute_kb"]), int(r["rep"]))][r["hashfn"]] = float(r["ns_per_hash"])
    net = defaultdict(list)   # (test, K, kb, hf) -> [net ns]
    for (t, k, kb, _), v in raw.items():
        for hf in HFS:
            if hf in v and "none" in v:
                net[(t, k, kb, hf)].append(v[hf] - v["none"])
    kbs = sorted({k[2] for k in net})
    ks = sorted({k[1] for k in net})
    tests = ["thr", "lat", "locate"]
    names = {"thr": "throughput\n(independent)", "lat": "latency\n(dependent chain)",
             "locate": "hash + bucket\nindices + signature"}

    fig, ax = plt.subplots(figsize=(6, 3.2))
    data = {(names[t], hf): net[(t, 0, kbs[0], hf)] for t in tests for hf in HFS}
    bars(ax, [names[t] for t in tests], data, "ns per hash (net of loop)")
    ax.set_title("Time per hash, tables warm (K = 0)", loc="left", fontsize=10)
    fig.savefig(os.path.join(D, "e2_speed.png"))
    plt.close(fig)

    # under cache pressure: the paired difference tab - crc within each repetition (same K, same buffer, run
    # back to back), steadier than each net time: the pollution loads' own variation cancels out
    diff = defaultdict(list)  # (test, K, kb) -> [tab - crc]
    for (t, k, kb, _), v in raw.items():
        if "tab" in v and "crc" in v:
            diff[(t, k, kb)].append(v["tab"] - v["crc"])
    fig, axs = plt.subplots(1, len(kbs), figsize=(6.4, 3), sharey=True, squeeze=False)
    tcol = {"thr": COL["tab"], "lat": "#00b894"}       # slots 1 and 3: two tests, not the two hashes
    tlab = {"thr": "throughput", "lat": "latency"}
    for ax, kb in zip(axs[0], kbs):
        for t in ("thr", "lat"):
            m = [med_rng(diff[(t, k, kb)]) for k in ks]
            ax.fill_between(ks, [x[1] for x in m], [x[2] for x in m], color=tcol[t], alpha=0.15, linewidth=0)
            ax.plot(ks, [x[0] for x in m], color=tcol[t], linewidth=2, marker="o", markersize=4, label=tlab[t])
        where = "L2" if kb <= 512 else "L3"
        ax.set_title(f"other lines from a {kb} KB buffer ({where})", loc="left", fontsize=9)
        ax.set_xlabel("random cache lines read per hash (K)")
        ax.set_xticks(ks)
        ax.axhline(0, color="#999999", linewidth=0.8)
    axs[0][0].set_ylabel("tab - crc, ns per hash")
    axs[0][0].legend(loc="upper left")
    fig.suptitle("Extra cost of tabulation over CRC32C when other data competes for L1", x=0.02, ha="left",
                 fontsize=10)
    fig.savefig(os.path.join(D, "e2_speed_pollution.png"))
    plt.close(fig)

    summary.append("## Time per hash (net of the loop, ns; median [min-max] over repetitions)\n")
    summary.append("| test | K | buffer KB | tab | crc | tab - crc (paired) |\n|---|---|---|---|---|---|")
    for t in tests:
        for kb in kbs:
            for k in ks:
                c = []
                for hf in HFS:
                    m, a, b = med_rng(net[(t, k, kb, hf)])
                    c.append(f"{m:.2f} [{a:.2f}-{b:.2f}]")
                dm, da, db = med_rng(diff[(t, k, kb)])
                summary.append(f"| {t} | {k} | {kb} | {c[0]} | {c[1]} | {dm:+.2f} [{da:+.2f}-{db:+.2f}] |")
    summary.append("")

# ---- spread ----
sr = rows("spread.csv")
if sr:
    dists = list(dict.fromkeys(r["dist"] for r in sr))
    fig, axs = plt.subplots(1, 2, figsize=(7.2, 3))
    ax = axs[0]
    ax.axhspan(-2, 2, color="#eeeeee", linewidth=0)
    ax.annotate("as random (|z| < 2)", (-0.4, 2.3), ha="left", va="bottom", fontsize=7, color=TXT2)
    for j, hf in enumerate(HFS):
        for i, d in enumerate(dists):
            z = [float(r["z_b1"]) for r in sr if r["dist"] == d and r["hashfn"] == hf]
            ax.scatter([i + (j - 0.5) * 0.3] * len(z), z, s=14, color=COL[hf], edgecolor="white", linewidth=0.6,
                       label=LBL[hf] if i == 0 else None, zorder=3)
    ax.set_yscale("symlog", linthresh=10)
    ax.set_yticks([-100, -10, -2, 0, 2, 10])
    ax.set_yticklabels(["-100", "-10", "-2", "0", "2", "10"])
    ax.set_xticks(range(len(dists)))
    ax.set_xticklabels(dists, rotation=20)
    ax.set_ylabel("chi-square z, first bucket")
    ax.set_title("Bucket spread (0 = as random; < 0 too even)", loc="left", fontsize=9)
    ax.grid(axis="x", visible=False)
    ax.legend(loc="lower left")

    ax = axs[1]
    data = {(d, hf): [float(r["same_b1b2sig"]) for r in sr if r["dist"] == d and r["hashfn"] == hf]
            for d in dists for hf in HFS}
    bars(ax, dists, data, "key pairs", fmt="{:.0f}", legend=None)
    ax.set_xticklabels(dists, rotation=20)
    ax.set_title("Pairs with same 2 buckets AND signature", loc="left", fontsize=9)
    fig.savefig(os.path.join(D, "e2_spread.png"))
    plt.close(fig)

    summary.append("## Spread (median over seeds)\n")
    summary.append("| dist | hash | chi2/df b1 | z b1 | max b1 | z b2 | z sig | 32-bit collisions (exp.) | "
                   "same b1,b2,sig (exp.) |\n|---|---|---|---|---|---|---|---|---|")
    for d in dists:
        for hf in HFS:
            g = [r for r in sr if r["dist"] == d and r["hashfn"] == hf]
            if not g:
                continue
            md = lambda k: st.median(float(r[k]) for r in g)  # noqa: E731
            summary.append(f"| {d} | {hf} | {md('chi2_b1') / (int(g[0]['nb']) - 1):.3f} | {md('z_b1'):.1f} | "
                           f"{md('max_b1'):.0f} | {md('z_b2'):.1f} | {md('z_sig'):.1f} | "
                           f"{md('coll32'):.0f} ({md('coll32_expected'):.0f}) | "
                           f"{md('same_b1b2sig'):.0f} ({md('same_b1b2sig_expected'):.3f}) |")
    summary.append("")

# ---- fill: load factor at the first failure, displacements per insert ----
fr = rows("fill.csv")
if fr:
    ends = [r for r in fr if r["kind"] != "curve"]
    dists = list(dict.fromkeys(r["dist"] for r in ends))
    lay = ends[0]
    layout = f"{lay['nb']} buckets x {lay['bs']}, depth {lay['depth']}, budget {lay['budget']}"
    fig, axs = plt.subplots(1, 2, figsize=(7.2, 3))
    data = {(d, hf): [float(r["load_at_failure"]) for r in ends if r["dist"] == d and r["hashfn"] == hf]
            for d in dists for hf in HFS}
    bars(axs[0], dists, data, "load factor at first failed insert", fmt=None, legend="lower right")
    lo = min(min(v) for v in data.values() if v)
    axs[0].set_ylim(max(0, lo - 0.05), 1.0)
    axs[0].set_xticklabels(dists, rotation=20)
    axs[0].set_title("Load factor reached", loc="left", fontsize=9)

    ax = axs[1]
    curves = [r for r in fr if r["kind"] == "curve"]
    for hf in HFS:
        for d, ls in (("mixed", "-"), ("nat", ":")):
            pts = defaultdict(list)
            for r in curves:
                if r["hashfn"] == hf and r["dist"] == d:
                    pts[float(r["load_lo"])].append(float(r["displacements_per_insert"]))
            if not pts:
                continue
            xs = sorted(pts)
            ax.plot([x + 0.025 for x in xs], [st.median(pts[x]) for x in xs], ls, color=COL[hf], linewidth=2,
                    label=f"{LBL[hf]}, {d}")
    ax.set_yscale("symlog", linthresh=0.01)
    ax.set_xlabel("load factor")
    ax.set_ylabel("displacements per insert")
    ax.set_title("Displacements as the table fills", loc="left", fontsize=9)
    ax.legend(fontsize=7)
    fig.suptitle(f"Effect on E1's load factor ({layout})", x=0.02, y=1.04, ha="left", fontsize=10)
    fig.savefig(os.path.join(D, "e2_fill.png"))
    plt.close(fig)

    summary.append(f"## Load factor at the first failed insert ({layout}; median [min-max] over seeds)\n")
    summary.append("| dist | tab | crc |\n|---|---|---|")
    for d in dists:
        c = []
        for hf in HFS:
            v = data[(d, hf)]
            c.append(f"{st.median(v):.4f} [{min(v):.4f}-{max(v):.4f}]" if v else "-")
        summary.append(f"| {d} | {c[0]} | {c[1]} |")
    if any(r["kind"] == "no_failure" for r in ends):
        summary.append("\n(no_failure rows: the key set ran out before the table failed)")
    summary.append("")

# ---- bench: the LB in the harness's bench mode ----
br = rows("bench.csv")
if br:
    dists = list(dict.fromkeys(r["dist"] for r in br))
    stages = [("hash_ns", "hash"), ("lookup_ns", "lookup"), ("insert_ns", "insert"), ("total_ns", "total")]
    fig, axs = plt.subplots(1, len(stages), figsize=(9, 2.8), sharey=False)
    for ax, (k, name) in zip(axs, stages):
        data = {(d, hf): [float(r[k]) for r in br if r["dist"] == d and r["hashfn"] == hf]
                for d in dists for hf in HFS}
        bars(ax, [d.replace("sequential_ports", "seq_ports") for d in dists],
             {(g.replace("sequential_ports", "seq_ports"), hf): v for (g, hf), v in data.items()},
             "ns per packet" if k == "hash_ns" else "", fmt=None, legend=None)
        ax.set_title(name, loc="left", fontsize=9)
        ax.tick_params(axis="x", labelsize=7)
    h, l = axs[0].get_legend_handles_labels()
    fig.legend(h, l, loc="upper right", ncol=2, bbox_to_anchor=(1.0, 1.06))
    fig.suptitle("The LB in the harness's bench mode: stage time per packet", x=0.02, y=1.04, ha="left", fontsize=10)
    fig.savefig(os.path.join(D, "e2_bench.png"))
    plt.close(fig)

    summary.append("## LB in bench mode, ns per packet (median [min-max] over seeds)\n")
    summary.append("| dist | stage | tab | crc | tab - crc |\n|---|---|---|---|---|")
    for d in dists:
        for k, name in stages:
            v = {hf: [float(r[k]) for r in br if r["dist"] == d and r["hashfn"] == hf] for hf in HFS}
            m = {hf: med_rng(v[hf]) for hf in HFS}
            summary.append(f"| {d} | {name} | {m['tab'][0]:.2f} [{m['tab'][1]:.2f}-{m['tab'][2]:.2f}] | "
                           f"{m['crc'][0]:.2f} [{m['crc'][1]:.2f}-{m['crc'][2]:.2f}] | "
                           f"{m['tab'][0] - m['crc'][0]:+.2f} |")
    summary.append("")

# ---- the cache cost: computed ----
summary.append("""## Cache cost (computed)

| what | bytes | 64-B lines | read per packet |
|---|---|---|---|
| tabulation tables, 13 x 256 x 8 B | 26,624 | 416 (81 % of a 32 KiB L1d) | 13 random lines |
| CRC32C | 0 | 0 | 0 (5 CRC instructions, ~3 cycles each, dependent) |
| packet headers (Eth + IPv4 + TCP, 54 B) | 54 | 1-2 | 1-2 |
| a bucket of the index (bucket_size x 4 B) | 32-64 | 1-2 | 1-2 (2 buckets on a miss) |
| an entry (32 B) | 32 | 1 (half a line) | 1 on a signature match |
| one batch of 64 packets | | ~256-320 | |
""")

seedf = os.path.join(D, "seed.txt")
if os.path.exists(seedf):
    summary.append("## Do collisions survive a change of seed? (2M random keys, 32 low bits)\n")
    summary.append("```\n" + open(seedf).read() + "```\n")

machine = os.path.join(D, "machine.txt")
head = "# E2 summary\n\n" + ("```\n" + open(machine).read() + "```\n\n" if os.path.exists(machine) else "")
text = head + "\n".join(summary)
with open(os.path.join(D, "summary.md"), "w") as f:
    f.write(text)
print(text)
print(f"figures: {D}/e2_*.png", file=sys.stderr)

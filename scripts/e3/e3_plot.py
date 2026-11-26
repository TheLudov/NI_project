#!/usr/bin/env python3
"""E3, replica selection (Atieh): the figures and the summary table, from scripts/e3/run_e3.sh's results.

    python3 scripts/e3/e3_plot.py build/e3

Reads build/e3/ring.csv (the ring alone, 7 seeds) and build/e3/balance.csv (our LB, each service alone, no churn,
5 seeds) and writes:
    e3_balance.png   most loaded replica / average, against the points per replica v, per service
    e3_moved.png     share of the flows moved when one replica is added or removed, relative to the ideal
    e3_cost.png      memory of the rings, time of one replica event, time of one lookup (ring alone, and the extra
                     time per lookup in our LB compared with v = 1), against v
    summary.txt      the numbers behind the figures (medians, and min-max over the seeds), and the machine
Each point is the median over the seeds; the bars show the min-max spread. Needs matplotlib
(pip install matplotlib, or apt install python3-matplotlib).
"""
import csv
import statistics
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FixedLocator, FuncFormatter, NullLocator

OUT = sys.argv[1] if len(sys.argv) > 1 else "build/e3"
SERVICES = ["chat", "video", "web"]
VS = [1, 10, 100, 1000]
# categorical slots 1-4, in fixed order (validated light-mode palette); markers as the second encoding
BLUE, ORANGE, AQUA, YELLOW = "#257de0", "#f47230", "#00b894", "#f2a600"
TEXT, TEXT2, GRID = "#191919", "#666666", "#e5e5e5"

plt.rcParams.update({
    "font.size": 9, "axes.edgecolor": TEXT2, "axes.labelcolor": TEXT, "xtick.color": TEXT2, "ytick.color": TEXT2,
    "axes.spines.top": False, "axes.spines.right": False, "axes.grid": True, "grid.color": GRID,
    "grid.linewidth": 0.8, "axes.titleweight": "bold", "axes.titlesize": 10, "legend.frameon": False,
    "savefig.dpi": 200, "savefig.bbox": "tight",
})


def read(path):
    with open(path) as f:
        return list(csv.DictReader(f))


def stats(rows, key):
    """median, min, max of a column over the seeds"""
    vals = [float(r[key]) for r in rows]
    return statistics.median(vals), min(vals), max(vals)


ring = defaultdict(list)       # (service, v) -> rows, one per seed
for r in read(f"{OUT}/ring.csv"):
    ring[(r["service"], int(r["v"]))].append(r)
bal = defaultdict(list)
for r in read(f"{OUT}/balance.csv"):
    bal[(r["service"], int(r["v"]))].append(r)
contest = {}                   # service -> (packets, replica computations), from the contest scenario, if run
try:
    for f in csv.reader(open(f"{OUT}/contest.csv")):
        if len(f) >= 3 and f[1] in ("pkts_rx", "replica_computations"):
            contest.setdefault(f[0], {})[f[1]] = float(f[2])
except OSError:
    pass


def lb_extra_ns(svc, v):
    """Extra time per ring lookup in our LB compared with v = 1, one value per seed. The harness times
    compute_replica once per batch, even with nothing to compute, so time = calls * c0 + lookups * c(v): the same
    seed at v = 1 has the same calls and lookups (no churn), and its 1-point ring always stays in cache."""
    base = {r["seed"]: r for r in bal[(svc, 1)]}
    out = []
    for r in bal[(svc, v)]:
        b = base.get(r["seed"])
        n = float(r["cr_pkts"])
        if b and n >= 10000 and float(b["cr_pkts"]) == n:   # enough lookups to measure (not video)
            out.append((float(r["cr_time_ns"]) - float(b["cr_time_ns"])) / n)
    return out


def short(n):
    return f"{n / 1000:g}k" if n >= 1000 else str(n)


def xaxis(ax):
    ax.set_xscale("log")
    ax.xaxis.set_major_locator(FixedLocator(VS))
    ax.xaxis.set_minor_locator(NullLocator())
    ax.xaxis.set_major_formatter(FuncFormatter(lambda x, _: f"{int(x):d}"))
    ax.set_xlim(0.7, 1500)
    ax.set_xlabel("points per replica v")


def series(ax, rows_by_v, key, color, marker, label, dx=1.0, label_end=False):
    xs, med, lo, hi = [], [], [], []
    for v in VS:
        rows = rows_by_v.get(v)
        if not rows:
            continue
        m, a, b = stats(rows, key)
        xs.append(v * dx); med.append(m); lo.append(m - a); hi.append(b - m)
    ax.errorbar(xs, med, yerr=[lo, hi], color=color, marker=marker, markersize=6, linewidth=2, elinewidth=1,
                capsize=2, label=label, markeredgecolor="white", markeredgewidth=1)
    if label_end and xs:
        ax.annotate(f"{med[-1]:.2f}", (xs[-1], med[-1]), xytext=(6, 0), textcoords="offset points",
                    va="center", fontsize=8, color=TEXT)


# ---- figure 1: balance ----
fig, axes = plt.subplots(1, 3, figsize=(10.5, 3.6), sharey=True)
for ax, svc in zip(axes, SERVICES):
    rv = {v: ring[(svc, v)] for v in VS if ring[(svc, v)]}
    bv = {v: bal[(svc, v)] for v in VS if bal[(svc, v)]}
    n = rv[VS[0]][0]["N"] if rv else "?"
    nf = rv[VS[0]][0]["flows"] if rv else "?"
    series(ax, rv, "ring_maxavg", BLUE, "o", "ring alone: share of the hash space", 0.92)
    if bv:
        series(ax, bv, "flows_maxavg", ORANGE, "s", "our LB: flows", 0.97)
        series(ax, bv, "pkts_maxavg", AQUA, "^", "our LB: packets, whole run", 1.03)
        series(ax, bv, "sec_median", YELLOW, "D", "our LB: packets in one second (median)", 1.08)
    ax.axhline(1, color=TEXT2, linewidth=0.8, linestyle=":")
    ax.set_title(f"{svc}  (N = {n}, {short(int(nf))} flows)")
    xaxis(ax)
    ax.set_yscale("log")
    ticks = [1, 1.1, 1.25, 1.5, 2, 3, 4, 5]
    ax.yaxis.set_major_locator(FixedLocator(ticks))
    ax.yaxis.set_minor_locator(NullLocator())
    ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y:g}"))
    ax.set_ylim(0.97, 6)
axes[0].set_ylabel("most loaded replica / average")
h, l = axes[0].get_legend_handles_labels()
fig.legend(h, l, loc="upper center", ncol=4, bbox_to_anchor=(0.5, 1.07), fontsize=8.5)
fig.savefig(f"{OUT}/e3_balance.png")
plt.close(fig)

# ---- figure 2: flows moved, relative to the ideal ----
fig, axes = plt.subplots(1, 3, figsize=(10.5, 3.4), sharey=True)
for ax, svc in zip(axes, SERVICES):
    for kind, color, marker, label, dx in (("add", BLUE, "o", "one replica added (ideal 1/(N+1))", 0.95),
                                            ("rm", ORANGE, "s", "one replica removed (ideal 1/N)", 1.05)):
        xs, med, lo, hi = [], [], [], []
        for v in VS:
            rows = ring[(svc, v)]
            if not rows:
                continue
            ideal = float(rows[0][f"{kind}_ideal"])
            m = statistics.median(float(r[f"{kind}_mean"]) for r in rows) / ideal
            a = min(float(r[f"{kind}_min"]) for r in rows) / ideal
            b = max(float(r[f"{kind}_max"]) for r in rows) / ideal
            xs.append(v * dx); med.append(m); lo.append(m - a); hi.append(b - m)
        ax.errorbar(xs, med, yerr=[lo, hi], color=color, marker=marker, markersize=6, linewidth=2, elinewidth=1.2,
                    capsize=3, label=label, markeredgecolor="white", markeredgewidth=1)
    ax.axhline(1, color=TEXT2, linewidth=0.8, linestyle=":")
    ax.set_title(f"{svc}  (N = {ring[(svc, 1)][0]['N']})")
    xaxis(ax)
axes[0].set_ylabel("flows moved / ideal  (mean; bars: min-max)")
h, l = axes[0].get_legend_handles_labels()
fig.legend(h, l, loc="upper center", ncol=2, bbox_to_anchor=(0.5, 1.07), fontsize=8.5)
fig.savefig(f"{OUT}/e3_moved.png")
plt.close(fig)

# ---- figure 3: memory, event time, lookup time (four charts, one axis each) ----
fig, ((a1, a2), (a3, a4)) = plt.subplots(2, 2, figsize=(9, 6.4))
for svc, color, marker in zip(SERVICES, (BLUE, ORANGE, AQUA), ("o", "s", "^")):
    xs = [v for v in VS if ring[(svc, v)]]
    def med(key, scale=1.0):
        return [statistics.median(float(r[key]) for r in ring[(svc, v)]) * scale for v in xs]
    for ax, ys in ((a1, med("mem_bytes", 1 / 1024)), (a2, med("add_event_us")), (a3, med("lookup_ns"))):
        ax.plot(xs, ys, color=color, marker=marker, markersize=6, linewidth=2, label=svc, markeredgecolor="white")
    lx, lm, llo, lhi = [], [], [], []
    for v in VS[1:]:
        d = lb_extra_ns(svc, v)
        if d:
            m = statistics.median(d)
            # a whisker below 1 ns (a negative difference: noise, chat has few lookups) is cut at the axis
            lx.append(v); lm.append(m); llo.append(m - max(min(d), 1.0)); lhi.append(max(d) - m)
    if lx:
        a4.errorbar(lx, lm, yerr=[llo, lhi], color=color, marker=marker, markersize=6, linewidth=2, elinewidth=1,
                    capsize=2, label=svc, markeredgecolor="white")
for ax in (a1, a2, a3, a4):
    xaxis(ax)
for ax in (a1, a2, a4):
    ax.set_yscale("log")
    ax.yaxis.set_major_formatter(FuncFormatter(lambda y, _: f"{y:g}"))
a1.set_ylabel("KB per service")
a1.set_title("ring memory")
a2.set_ylabel("\u00b5s")
a2.set_title("one replica added (an event)")
a3.set_ylabel("ns")
a3.set_ylim(bottom=0)
a3.set_title("one lookup, ring alone (hot cache)")
a4.set_ylabel("ns")
a4.set_title("one lookup in our LB: extra time vs v = 1")
a4.text(0.98, 0.04, "video: too few lookups to measure", transform=a4.transAxes, ha="right", fontsize=8,
        color=TEXT2)
a1.legend(loc="upper left", fontsize=8.5)
a4.legend(loc="upper left", fontsize=8.5)
fig.tight_layout()
fig.savefig(f"{OUT}/e3_cost.png")
plt.close(fig)

# ---- summary table ----
with open(f"{OUT}/summary.txt", "w") as f:
    def w(s=""):
        f.write(s + "\n")
    w("E3 summary: medians over the seeds (min-max in brackets). ring: ring alone, 7 seeds; LB: our LB, no churn.")
    w()
    for svc in SERVICES:
        r1 = ring[(svc, 1)][0]
        w(f"{svc}: N = {r1['N']} replicas, {int(r1['flows']):,} concurrent flows")
        w(f"  {'v':>5} {'ring share':>17} {'flows (ring, real n)':>21} {'flows (LB)':>17} {'pkts (LB)':>17} "
          f"{'pkts/s median':>14} {'add moved/ideal':>16} {'rm max/ideal':>13} {'mem KB':>8} {'lookup ns':>10} "
          f"{'lookups/pkt':>12} {'LB extra ns':>15} {'add us':>7} {'rm us':>6}")
        for v in VS:
            rr, bb = ring[(svc, v)], bal[(svc, v)]
            if not rr:
                continue
            def fmt(rows, key, d=3):
                if not rows:
                    return "-"
                m, a, b = stats(rows, key)
                return f"{m:.{d}f} ({a:.{d}f}-{b:.{d}f})"
            ideal_add, ideal_rm = float(rr[0]["add_ideal"]), float(rr[0]["rm_ideal"])
            add = statistics.median(float(r["add_mean"]) for r in rr) / ideal_add
            addmx = max(float(r["add_max"]) for r in rr) / ideal_add
            rmmx = max(float(r["rm_max"]) for r in rr) / ideal_rm
            mem = statistics.median(float(r["mem_bytes"]) for r in rr) / 1024
            ns = statistics.median(float(r["lookup_ns"]) for r in rr)
            miss = f"{100 * statistics.median(float(r['miss_fraction']) for r in bb):.3g} %" if bb else "-"
            ex = lb_extra_ns(svc, v) if v > 1 else []
            lbns = f"{statistics.median(ex):.0f} ({min(ex):.0f}-{max(ex):.0f})" if ex else "-"
            ev_add = statistics.median(float(r["add_event_us"]) for r in rr)
            ev_rm = statistics.median(float(r["rm_event_us"]) for r in rr)
            w(f"  {v:>5} {fmt(rr, 'ring_maxavg'):>17} {fmt(rr, 'flows_maxavg'):>21} {fmt(bb, 'flows_maxavg'):>17} "
              f"{fmt(bb, 'pkts_maxavg'):>17} {fmt(bb, 'sec_median', 2):>14} {add:>7.2f} (max {addmx:.2f}) "
              f"{rmmx:>13.2f} {mem:>8.1f} {ns:>10.1f} {miss:>12} {lbns:>15} {ev_add:>7.1f} {ev_rm:>6.1f}")
        wrong = sum(int(r["moved_wrong"]) for v in VS for r in ring[(svc, v)])
        w(f"  flows that moved anywhere but to the added replica, or from a replica not removed: {wrong}")
        c = contest.get(svc)
        if c and c.get("pkts_rx"):
            w(f"  contest scenario (mixed_flood): {c['replica_computations']:,.0f} lookups for {c['pkts_rx']:,.0f} "
              f"packets = {100 * c['replica_computations'] / c['pkts_rx']:.3g} % of its packets")
        w()
    w("Columns: ring share = largest share of the hash space / 1/N (ring alone); flows (ring, real n) = the same")
    w("with the service's real number of concurrent flows; LB columns = our LB, each service alone, no churn;")
    w("pkts/s median = median over the seconds of (packets of the most loaded replica in that second / average);")
    w("lookups/pkt = replica computations / packets (flow-table misses); LB extra ns = extra time of one lookup in")
    w("our LB compared with v = 1, from compute_replica's profile (calls and lookups are the same at every v for a")
    w("seed; not measurable for video, which almost never computes a replica); add/rm us = time to apply one")
    w("replica event to the ring.")
    try:
        with open(f"{OUT}/machine.txt") as m:
            w()
            w("Machine:")
            for line in m:
                w("  " + line.rstrip())
    except OSError:
        pass
print(f"wrote {OUT}/e3_balance.png, e3_moved.png, e3_cost.png, summary.txt")

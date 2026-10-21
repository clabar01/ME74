"""Plot the field-test trials (0-90 m tape) from ME74_field_tests_consolidated.xlsx.

Build the workbook first with analysis/build_workbook.py, then run this in VS Code (Run Python File) or:
    python3 analysis/plot_field_tests.py [path/to/ME74_field_tests_consolidated.xlsx]
Graphs are saved to analysis/graphs/.
"""
import os, sys
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

XLSX = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/Downloads/ME74_field_tests_consolidated.xlsx")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "graphs")
os.makedirs(OUT, exist_ok=True)

# trial tab -> (label, walked 90 -> 0?)
TRIALS = {"Day 1 ESP-NOW walk": ("Day 1 ESP-NOW walk", False), "Day 2 Trial 1": ("Day 2 Trial 1", False),
          "Day 2 Trial 3": ("Day 2 Trial 3", False), "Day 2 Trial 4": ("Day 2 Trial 4", True),
          "Day 2 Return run": ("Day 2 Return run", True)}
LIN = 100                                   # linear scale up to 100 m, log scale beyond (radio/IMU estimates reach the 1000s)


def tape_axis(ax, values, compact=False):
    """Linear 0-100 m so the tape range stays readable, log above/below so large errors stay visible."""
    v = pd.concat(values).dropna()
    lo, hi = min(-5, v.min() * 1.3), max(100, v.max() * 1.3)
    ax.set_yscale("symlog", linthresh=LIN, linscale=3)
    ax.set_ylim(lo, hi)
    neg = (-10000, -1000, -100, -50) if compact else (-5000, -2000, -1000, -500, -200, -100, -50)
    pos = (100, 1000, 10000) if compact else (100, 200, 500, 1000, 2000, 5000, 10000)
    ticks = [t for t in neg if t > lo] + list(range(0, 100, 20 if compact else 10)) + [t for t in pos if t < hi]
    ax.set_yticks(ticks, [str(t) for t in ticks])
    ax.yaxis.set_minor_locator(matplotlib.ticker.NullLocator())
    if hi > LIN:
        ax.axhspan(LIN, hi, color="#f4f3ef", zorder=0)
    if lo < -LIN:
        ax.axhspan(lo, -LIN, color="#f4f3ef", zorder=0)

# sensor -> (column, colour). GPS/IMU are distance travelled since zero; radios are distance to the base node at 0 m.
SENSORS = {
    "GPS":     ("GPS dist from zero (m)", "#2a78d6"),
    "LoRa":    ("LoRa est dist (m)", "#eb6834"),
    "ESP-NOW": ("ESP-NOW est dist (m)", "#1baf7a"),
    "IMU1":    ("IMU1 dist from zero (m)", "#eda100"),
    "IMU2":    ("IMU2 dist from zero (m)", "#e87ba4"),
}
TRIAL_COLORS = {"Day 1 ESP-NOW walk": "#5f5e5a", "Day 2 Trial 1": "#2a78d6", "Day 2 Trial 3": "#eb6834",
                "Day 2 Trial 4": "#1baf7a", "Day 2 Return run": "#e87ba4"}

plt.rcParams.update({"axes.spines.top": False, "axes.spines.right": False, "axes.grid": True,
                     "grid.color": "#e4e3df", "grid.linewidth": 0.8, "axes.edgecolor": "#8a8984",
                     "font.size": 10, "axes.titleweight": "bold", "figure.dpi": 130})

book = pd.read_excel(XLSX, sheet_name=None)

# 1) one time-series figure per trial: each sensor's estimate of position on the tape
for tab, (label, reverse) in TRIALS.items():
    df = book[tab]
    # break the lines where logging paused (e.g. between Day 1 segment files) instead of drawing across the gap
    gap = df["Time (s)"].diff() > 3
    breaks = df.loc[gap, ["Time (s)"]] - 0.5
    df = pd.concat([df, breaks]).sort_values("Time (s)", kind="stable")
    fig, ax = plt.subplots(figsize=(11, 5.5))
    plotted = []
    for name, (col, color) in SENSORS.items():
        if col not in df:
            continue
        v = df[col]
        if reverse and name in ("GPS", "IMU1", "IMU2"):
            v = 90 - v
        ax.plot(df["Time (s)"], v, color=color, lw=2, label=name)
        plotted.append(v)
    tape_axis(ax, plotted)
    for seg, t in df.groupby("Segment (m)", sort=False)["Time (s)"].first().items():
        ax.axvline(t, color="#b5b4ad", lw=0.8, ls="--")
        ax.text(t + 1, 1.01, seg, fontsize=8, color="#5f5e5a", va="bottom", transform=ax.get_xaxis_transform())
    ax.set_xlabel("Time since trial start (s)")
    ax.set_ylabel("Estimated position on tape (m)")
    ax.set_title(f"{label} ({'90 → 0' if reverse else '0 → 90'} m): sensor estimates over time", pad=18)
    ax.legend(ncol=5, loc="upper center", bbox_to_anchor=(0.5, -0.12), frameon=False)
    fig.text(0.01, 0.01, f"Shaded area: log scale beyond ±{LIN} m.", fontsize=8, color="#5f5e5a")
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, f"{tab.replace(' ', '_')}_timeseries.png"))
    plt.close(fig)

# segment-end estimates (segments with no recorded movement are left out)
summ = book["Segment summary"]
summ = summ[summ["Note"].fillna("") == ""]

# 2) estimated vs true position at each segment end, one panel per sensor, all trials together
fig, axes = plt.subplots(1, 5, figsize=(18, 4.6), sharex=True, sharey=True)
for ax, name in zip(axes, SENSORS):
    ax.plot([0, 90], [0, 90], color="#5f5e5a", lw=1, ls="--", label="Perfect")
    for trial, e in summ.groupby("Trial", sort=False):
        tab = [k for k in TRIALS if k.endswith(trial)][0]
        e = e.dropna(subset=[f"{name} est position (m)"]).sort_values("True position (m)")
        if len(e):
            ax.plot(e["True position (m)"], e[f"{name} est position (m)"], "o-",
                    color=TRIAL_COLORS[tab], lw=1.5, ms=5, label=TRIALS[tab][0])
    ax.set_title(name)
    ax.set_xlabel("True position (m)")
    ax.set_xticks(range(0, 100, 30))
cols = [f"{n} est position (m)" for n in SENSORS]
for ax in axes:
    tape_axis(ax, [summ[c] for c in cols], compact=True)
axes[0].set_ylabel("Estimated position (m)")
handles, labels = axes[2].get_legend_handles_labels()
fig.legend(handles, labels, ncol=6, loc="lower center", frameon=False)
fig.suptitle("Estimated vs true position at the end of each 10 m segment (all trials)", fontweight="bold")
fig.text(0.01, 0.01, f"Shaded area: log scale beyond ±{LIN} m.", fontsize=8, color="#5f5e5a")
fig.tight_layout(rect=(0, 0.07, 1, 1))
fig.savefig(os.path.join(OUT, "all_trials_estimated_vs_true.png"))
plt.close(fig)

# 3) GPS error at each segment end
fig, ax = plt.subplots(figsize=(9, 4.8))
ax.axhline(0, color="#5f5e5a", lw=1)
for trial, e in summ.dropna(subset=["GPS error (m)"]).groupby("Trial", sort=False):
    tab = [k for k in TRIALS if k.endswith(trial)][0]
    e = e.sort_values("True position (m)")
    ax.plot(e["True position (m)"], e["GPS error (m)"], "o-", color=TRIAL_COLORS[tab], lw=2, ms=6, label=TRIALS[tab][0])
ax.set_xticks(range(0, 100, 10))
ax.set_xlabel("True position (m)")
ax.set_ylabel("GPS error (m)")
ax.set_title("GPS error at each segment end (Day 2)")
ax.legend(frameon=False, ncol=4, loc="upper center", bbox_to_anchor=(0.5, -0.15))
fig.tight_layout()
fig.savefig(os.path.join(OUT, "gps_error_by_segment.png"))
plt.close(fig)

# 4) median absolute error per sensor per trial (median so a few radio blow-ups don't dominate; log scale)
err = {}
for trial, e in summ.groupby("Trial", sort=False):
    tab = [k for k in TRIALS if k.endswith(trial)][0]
    err[TRIALS[tab][0]] = {n: e[f"{n} error (m)"].abs().median() for n in SENSORS}
med = pd.DataFrame(err)
fig, ax = plt.subplots(figsize=(10, 4.8))
w = 0.8 / len(med.columns)
for i, trial in enumerate(med.columns):
    xs = [j + (i - (len(med.columns) - 1) / 2) * w for j in range(len(SENSORS))]
    tab = [k for k in TRIALS if TRIALS[k][0] == trial][0]
    ax.bar(xs, med[trial], width=w - 0.03, color=TRIAL_COLORS[tab], label=trial)
ax.set_yscale("log")
ax.set_xticks(range(len(SENSORS)), list(SENSORS))
ax.set_ylabel("Median absolute error (m, log scale)")
ax.set_title("Typical error at segment ends, by sensor and trial")
ax.legend(frameon=False, ncol=3, loc="upper center", bbox_to_anchor=(0.5, -0.1))
fig.tight_layout()
fig.savefig(os.path.join(OUT, "median_error_by_sensor.png"))
plt.close(fig)

med.round(2).to_csv(os.path.join(OUT, "median_error_by_sensor.csv"))
print(med.round(2).to_string())
print(f"\nSaved graphs to {OUT}")

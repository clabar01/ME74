"""Plot the Data 2 field-test trials (0-90 m tape) from the consolidated workbook.

Run in VS Code (Run Python File) or:  python3 analysis/plot_data2.py [path/to/ME74_Data2_consolidated.xlsx]
Graphs are saved to analysis/data2_graphs/.
"""
import os, sys
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

XLSX = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/Downloads/Data 2/ME74_Data2_consolidated.xlsx")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data2_graphs")
os.makedirs(OUT, exist_ok=True)

TRIALS = ["Trial 1", "Trial 2", "Trial 3", "Trial 4", "Return run"]
RETURN = {"Trial 4", "Return run"}          # walked 90 -> 0
Y_MAX = 120                                 # radio estimates spike into the 1000s; clip so the tape range stays readable

# sensor -> (from-zero column, colour). GPS/IMU are distance travelled since zero;
# radios are distance to the base node at the 0 m mark.
SENSORS = {
    "GPS":     ("GPS dist from zero (m)", "#2a78d6"),
    "LoRa":    ("LoRa est dist (m)", "#eb6834"),
    "ESP-NOW": ("ESP-NOW est dist (m)", "#1baf7a"),
    "IMU1":    ("IMU1 dist from zero (m)", "#eda100"),
    "IMU2":    ("IMU2 dist from zero (m)", "#e87ba4"),
}
TRIAL_COLORS = {"Trial 1": "#2a78d6", "Trial 3": "#eb6834", "Trial 4": "#1baf7a", "Return run": "#e87ba4"}

plt.rcParams.update({"axes.spines.top": False, "axes.spines.right": False, "axes.grid": True,
                     "grid.color": "#e4e3df", "grid.linewidth": 0.8, "axes.edgecolor": "#8a8984",
                     "font.size": 10, "axes.titleweight": "bold", "figure.dpi": 130})


def tape_position(df, trial):
    """Each sensor's estimate of position along the tape (0-90 m)."""
    pos = pd.DataFrame(index=df.index)
    for name, (col, _) in SENSORS.items():
        v = df[col]
        if trial in RETURN and name not in ("LoRa", "ESP-NOW"):
            v = 90 - v
        pos[name] = v
    return pos


def segment_ends(df):
    """Last valid reading of each segment and the true tape mark there."""
    rows = []
    for seg, g in df.groupby("Segment (m)", sort=False):
        end = int(seg.split("-")[1])
        r = {"Segment": seg, "True (m)": end}
        for name in SENSORS:
            v = g[name].dropna()
            r[name] = v.iloc[-1] if len(v) else None
        rows.append(r)
    return pd.DataFrame(rows)


book = pd.read_excel(XLSX, sheet_name=None)
ends = {}

# 1) one time-series figure per trial
for trial in TRIALS:
    df = book[trial]
    pos = tape_position(df, trial)
    full = pd.concat([df[["Time (s)", "Segment (m)"]], pos], axis=1)
    ends[trial] = segment_ends(full)

    fig, ax = plt.subplots(figsize=(11, 5.5))
    for name, (_, color) in SENSORS.items():
        ax.plot(full["Time (s)"], full[name].clip(upper=Y_MAX), color=color, lw=2, label=name)
    # mark where each segment label starts
    starts = full.groupby("Segment (m)", sort=False)["Time (s)"].first()
    for seg, t in starts.items():
        ax.axvline(t, color="#b5b4ad", lw=0.8, ls="--")
        ax.text(t + 1, Y_MAX - 3, seg, fontsize=8, color="#5f5e5a", va="top")
    ax.set_ylim(-5, Y_MAX)
    ax.set_yticks(range(0, 100, 10))
    ax.set_xlabel("Time since trial start (s)")
    ax.set_ylabel("Estimated position on tape (m)")
    direction = "90 → 0 m" if trial in RETURN else "0 → 90 m"
    ax.set_title(f"{trial} ({direction}): sensor estimates over time")
    ax.legend(ncol=5, loc="upper center", bbox_to_anchor=(0.5, -0.12), frameon=False)
    fig.text(0.01, 0.01, f"Values above {Y_MAX} m are clipped to the top edge.", fontsize=8, color="#5f5e5a")
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, f"{trial.replace(' ', '_')}_timeseries.png"))
    plt.close(fig)

# 2) estimated vs true position at each segment end, one panel per sensor, all complete trials together
compare = [t for t in TRIALS if t != "Trial 2"]
fig, axes = plt.subplots(1, 5, figsize=(18, 4.4), sharex=True, sharey=True)
for ax, name in zip(axes, SENSORS):
    ax.plot([0, 90], [0, 90], color="#5f5e5a", lw=1, ls="--", label="Perfect")
    for trial in compare:
        e = ends[trial]
        ax.plot(e["True (m)"], e[name].clip(upper=Y_MAX), "o-", color=TRIAL_COLORS[trial], lw=1.5, ms=5, label=trial)
    ax.set_title(name)
    ax.set_xlabel("True position (m)")
    ax.set_xticks(range(0, 100, 30))
    ax.set_ylim(-5, Y_MAX)
axes[0].set_ylabel("Estimated position (m)")
axes[0].legend(frameon=False, fontsize=8, loc="upper left")
fig.suptitle("Estimated vs true position at the end of each 10 m segment (all 0-90 trials)", fontweight="bold")
fig.tight_layout()
fig.savefig(os.path.join(OUT, "all_trials_estimated_vs_true.png"))
plt.close(fig)

# 3) GPS error at each segment end, all complete trials
fig, ax = plt.subplots(figsize=(9, 4.8))
ax.axhline(0, color="#5f5e5a", lw=1)
for trial in compare:
    e = ends[trial]
    ax.plot(e["True (m)"], e["GPS"] - e["True (m)"], "o-", color=TRIAL_COLORS[trial], lw=2, ms=6, label=trial)
ax.set_xticks(range(10, 100, 10))
ax.set_xlabel("True position (m)")
ax.set_ylabel("GPS error (m)")
ax.set_title("GPS error at each segment end")
ax.legend(frameon=False, ncol=4, loc="upper center", bbox_to_anchor=(0.5, -0.15))
fig.tight_layout()
fig.savefig(os.path.join(OUT, "gps_error_by_segment.png"))
plt.close(fig)

# 4) mean absolute error per sensor per trial (log scale: radios/IMU errors span 1-1000+ m)
mae = pd.DataFrame({t: {n: (ends[t][n] - ends[t]["True (m)"]).abs().mean() for n in SENSORS} for t in compare})
fig, ax = plt.subplots(figsize=(10, 4.8))
w = 0.2
for i, trial in enumerate(compare):
    xs = [j + (i - 1.5) * w for j in range(len(SENSORS))]
    ax.bar(xs, mae[trial], width=w - 0.03, color=TRIAL_COLORS[trial], label=trial)
ax.set_yscale("log")
ax.set_xticks(range(len(SENSORS)), list(SENSORS))
ax.set_ylabel("Mean absolute error (m, log scale)")
ax.set_title("Average error at segment ends, by sensor and trial")
ax.legend(frameon=False, ncol=4, loc="upper center", bbox_to_anchor=(0.5, -0.1))
fig.tight_layout()
fig.savefig(os.path.join(OUT, "mean_error_by_sensor.png"))
plt.close(fig)

mae.round(2).to_csv(os.path.join(OUT, "mean_error_by_sensor.csv"))
print(mae.round(2).to_string())
print(f"\nSaved graphs to {OUT}")

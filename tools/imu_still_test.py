#!/usr/bin/env python3
"""
IMU stationary test: record the IMU while it sits still, save a CSV, and
print mean, standard deviation and drift for every axis.

Works with BOTH firmware versions:
  - old firmware (old_firmware/IMU_old.cpp): reads its "Accel X: ..." printout
  - new firmware (IMU_initialtest.ino): switches it to logging mode ('l')

Typical use (run from the ME74 folder; PlatformIO's Python already has pyserial):

  1. Flash the OLD firmware and record:
       pio run -e old_firmware -t upload
       ~/.platformio/penv/bin/python tools/imu_still_test.py record --label old

  2. Flash the NEW firmware and record:
       pio run -e xiao_esp32s3 -t upload
       ~/.platformio/penv/bin/python tools/imu_still_test.py record --label new

  3. Compare the two runs side by side:
       ~/.platformio/penv/bin/python tools/imu_still_test.py compare data/still_old_*.csv data/still_new_*.csv

Close the Serial Monitor before recording (only one program can use the port).
Put the sensor on a solid, level surface and don't touch the table during the test.
"""

import argparse
import csv
import math
import re
import statistics
import sys
import time
from datetime import datetime
from pathlib import Path

DATA_DIR = Path(__file__).resolve().parent.parent / "data"

# CSV columns (old firmware leaves the angle/timing columns empty)
COLUMNS = ["t_s", "ax", "ay", "az", "gx", "gy", "gz", "mx", "my", "mz",
           "roll_deg", "pitch_deg", "heading_deg", "dt_ms", "overruns"]

# Which columns to analyze, and their units
AXES = [("ax", "m/s^2"), ("ay", "m/s^2"), ("az", "m/s^2"),
        ("gx", "rad/s"), ("gy", "rad/s"), ("gz", "rad/s"),
        ("mx", "uT"), ("my", "uT"), ("mz", "uT"),
        ("roll_deg", "deg"), ("pitch_deg", "deg"), ("heading_deg", "deg")]

OLD_LINE = re.compile(r"^(Accel|Gyro|Mag)\s+X:\s*(\S+)\s+Y:\s*(\S+)\s+Z:\s*(\S+)")


# =============================================================================
# Recording
# =============================================================================

def find_port():
    """Find the XIAO ESP32-S3 USB port automatically."""
    from serial.tools import list_ports
    known_vids = {0x2886, 0x303A}   # Seeed, Espressif
    ports = [p for p in list_ports.comports() if p.vid in known_vids]
    if len(ports) == 1:
        return ports[0].device
    all_ports = [p.device for p in list_ports.comports()]
    sys.exit(f"Could not pick the board's port automatically. Ports found: {all_ports}\n"
             f"Run again with --port <one of these>.")


def wait_for_calibration(ser, timeout_s=60):
    """Send '0' (re-zero / recalibrate) and wait until the firmware says it is done.
    Returns 'old' or 'new' depending on which firmware answered."""
    for attempt in range(2):
        ser.reset_input_buffer()
        ser.write(b"0")
        print("Calibrating... keep the sensor still.")
        end = time.monotonic() + timeout_s
        while time.monotonic() < end:
            line = ser.readline().decode(errors="ignore").strip()
            if "SENSOR ZEROED" in line:
                return "old"
            if "Calibration done" in line:
                return "new"
            if "Movement detected" in line:
                print("  Firmware detected movement, it is retrying...")
        print("  No answer from the board, trying again...")
    sys.exit("The board never confirmed calibration. Is the right firmware running "
             "and is the Serial Monitor closed?")


def record(args):
    import serial

    port = args.port or find_port()
    DATA_DIR.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_path = DATA_DIR / f"still_{args.label}_{stamp}.csv"

    print(f"Opening {port} ...")
    ser = serial.Serial(port, 115200, timeout=0.5)
    time.sleep(2.0)   # give the board time in case opening the port restarted it

    firmware = wait_for_calibration(ser)
    print(f"Detected {firmware.upper()} firmware.")
    if firmware == "new":
        ser.write(b"l")   # switch to logging mode (CSV lines starting with LOG,)

    duration_s = args.minutes * 60
    print(f"Recording for {args.minutes:g} min -> {out_path}")
    print("Don't touch the sensor or the table. Ctrl+C stops early (data is kept).\n")

    rows = 0
    t_start_host = time.monotonic()
    t0_us = None           # first board timestamp (new firmware)
    last_us = None
    wrap_us = 0            # micros() wraps every ~71 minutes
    old_sample = {}        # pieces of one old-firmware reading (Accel/Gyro/Mag lines)
    next_progress = 30

    with open(out_path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(COLUMNS)
        try:
            while True:
                elapsed = time.monotonic() - t_start_host
                if elapsed >= duration_s:
                    break
                if elapsed >= next_progress:
                    print(f"  {elapsed / 60:4.1f} min, {rows} samples")
                    next_progress += 30

                line = ser.readline().decode(errors="ignore").strip()
                if not line:
                    continue

                row = None
                if firmware == "new" and line.startswith("LOG,"):
                    parts = line.split(",")[1:]
                    if len(parts) != 15:
                        continue   # partial line
                    t_us = int(parts[0])
                    if last_us is not None and t_us < last_us:
                        wrap_us += 2 ** 32
                    last_us = t_us
                    if t0_us is None:
                        t0_us = t_us + wrap_us
                    t_s = (t_us + wrap_us - t0_us) / 1e6
                    row = [f"{t_s:.4f}"] + parts[1:]

                elif firmware == "old":
                    m = OLD_LINE.match(line)
                    if m:
                        old_sample[m.group(1)] = m.group(2, 3, 4)
                        if m.group(1) == "Mag" and len(old_sample) == 3:
                            # Old firmware has no timestamps, so use the computer's clock
                            t_s = time.monotonic() - t_start_host
                            row = ([f"{t_s:.4f}"] + list(old_sample["Accel"])
                                   + list(old_sample["Gyro"]) + list(old_sample["Mag"])
                                   + ["", "", "", "", ""])
                            old_sample = {}

                if row:
                    writer.writerow(row)
                    rows += 1
                    if rows % 200 == 0:
                        f.flush()
        except KeyboardInterrupt:
            print("\nStopped early.")

    if firmware == "new":
        ser.write(b"p")   # back to the normal printout
    ser.close()

    print(f"\nSaved {rows} samples to {out_path}\n")
    if rows < 10:
        sys.exit("Too few samples to analyze.")
    print_report(analyze(out_path))


# =============================================================================
# Analysis
# =============================================================================

def unwrap_degrees(values):
    """Remove 360-degree jumps (e.g. heading going 359 -> 1) so drift is measured correctly."""
    out = [values[0]]
    for v in values[1:]:
        step = v - out[-1]
        step -= 360 * round(step / 360)
        out.append(out[-1] + step)
    return out


def best_fit_drift(t, v):
    """Total change over the test according to a best-fit straight line.
    Using a fitted line means random noise doesn't count as drift."""
    tm, vm = statistics.fmean(t), statistics.fmean(v)
    num = sum((ti - tm) * (vi - vm) for ti, vi in zip(t, v))
    den = sum((ti - tm) ** 2 for ti in t)
    slope = num / den if den > 0 else 0.0
    return slope * (t[-1] - t[0])


def integrate_deg(t, rate_rad_s):
    """Integrate a gyro rate (rad/s) over time -> angle in degrees (trapezoid rule).
    This is the angle error you would get from raw gyro integration alone."""
    angle = 0.0
    for i in range(1, len(t)):
        angle += 0.5 * (rate_rad_s[i] + rate_rad_s[i - 1]) * (t[i] - t[i - 1])
    return math.degrees(angle)


def analyze(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    t = [float(r["t_s"]) for r in rows]
    result = {"file": str(path), "samples": len(rows), "duration_s": t[-1] - t[0],
              "firmware": "new" if rows[0]["roll_deg"] else "old", "axes": {}}

    for name, units in AXES:
        if not rows[0][name]:
            continue   # column not recorded by this firmware
        v = [float(r[name]) for r in rows]
        if name == "heading_deg":
            v = unwrap_degrees(v)
        result["axes"][name] = {
            "units": units,
            "mean": statistics.fmean(v) % 360 if name == "heading_deg" else statistics.fmean(v),
            "std": statistics.stdev(v),
            "drift": best_fit_drift(t, v),
        }

    # Angle error from integrating the gyro (comparable between old and new)
    result["gyro_integrated_deg"] = {
        ax: integrate_deg(t, [float(r[ax]) for r in rows]) for ax in ("gx", "gy", "gz")}

    # Timing consistency
    intervals = [(b - a) * 1000 for a, b in zip(t, t[1:])]
    result["interval_ms_mean"] = statistics.fmean(intervals)
    result["interval_ms_std"] = statistics.stdev(intervals)
    if result["firmware"] == "new":
        dts = [float(r["dt_ms"]) for r in rows]
        result["loop_dt_ms_mean"] = statistics.fmean(dts)
        result["loop_dt_ms_std"] = statistics.stdev(dts)
        result["overruns"] = int(rows[-1]["overruns"]) - int(rows[0]["overruns"])
    return result


def fmt(x):
    return f"{x:+.5f}" if abs(x) < 0.1 else f"{x:+.3f}"


def print_report(r):
    print(f"File:     {r['file']}")
    print(f"Firmware: {r['firmware']}   Samples: {r['samples']}   "
          f"Duration: {r['duration_s'] / 60:.2f} min")
    print(f"\n{'Axis':<12}{'Units':<8}{'Mean':>12}{'Std dev':>12}{'Drift':>12}")
    print("-" * 56)
    for name, a in r["axes"].items():
        print(f"{name:<12}{a['units']:<8}{fmt(a['mean']):>12}{a['std']:>12.5f}{fmt(a['drift']):>12}")

    print("\nAngle error from integrating the gyro alone (deg, over the whole test):")
    g = r["gyro_integrated_deg"]
    print(f"  about X: {g['gx']:+.2f}   about Y: {g['gy']:+.2f}   about Z: {g['gz']:+.2f}")

    print(f"\nSample interval: {r['interval_ms_mean']:.2f} ms average, "
          f"{r['interval_ms_std']:.2f} ms std dev"
          + ("  (old firmware: measured by the computer, includes USB delays)"
             if r["firmware"] == "old" else ""))
    if r["firmware"] == "new":
        print(f"Loop dt on the board: {r['loop_dt_ms_mean']:.3f} ms average, "
              f"{r['loop_dt_ms_std']:.3f} ms std dev, {r['overruns']} overruns")
    print("\nDrift = total change over the test from a best-fit line (noise removed).")


def compare(args):
    old, new = analyze(args.old_csv), analyze(args.new_csv)
    print(f"OLD: {old['file']}  ({old['samples']} samples, {old['duration_s'] / 60:.2f} min)")
    print(f"NEW: {new['file']}  ({new['samples']} samples, {new['duration_s'] / 60:.2f} min)\n")

    print(f"{'Axis':<8}{'Units':<8}{'Mean old':>11}{'Mean new':>11}"
          f"{'Std old':>11}{'Std new':>11}{'Drift old':>11}{'Drift new':>11}")
    print("-" * 82)
    for name, units in AXES[:9]:   # sensor axes recorded by both versions
        o, n = old["axes"][name], new["axes"][name]
        print(f"{name:<8}{units:<8}{fmt(o['mean']):>11}{fmt(n['mean']):>11}"
              f"{o['std']:>11.5f}{n['std']:>11.5f}{fmt(o['drift']):>11}{fmt(n['drift']):>11}")

    print("\nOrientation error over the test (deg):")
    print(f"{'':<10}{'OLD: gyro integrated':>22}{'NEW: fused angle drift':>25}")
    pairs = [("Roll", "gx", "roll_deg"), ("Pitch", "gy", "pitch_deg"),
             ("Heading", "gz", "heading_deg")]
    for label, gyro_axis, angle in pairs:
        old_err = old["gyro_integrated_deg"][gyro_axis]
        if label == "Heading":
            old_err = -old_err   # heading is clockwise, gyro Z is counter-clockwise
        new_err = new["axes"][angle]["drift"]
        print(f"{label:<10}{old_err:>+22.2f}{new_err:>+25.2f}")

    print(f"\nSample interval std dev: old {old['interval_ms_std']:.2f} ms (computer clock), "
          f"new {new['loop_dt_ms_std']:.3f} ms (board clock), {new['overruns']} overruns")
    print("\nNotes:")
    print(" - Old firmware subtracts gravity and re-zeros, so its accel means are ~0.")
    print("   New firmware keeps gravity (needed for roll/pitch), so new az is ~9.81.")
    print("   Compare std dev and drift for the accel, not the mean.")
    print(" - The old firmware never computed angles; its row shows the angle error you")
    print("   would get by integrating its gyro output directly.")


# =============================================================================

def main():
    parser = argparse.ArgumentParser(description="IMU stationary noise/drift test")
    sub = parser.add_subparsers(dest="command", required=True)

    p_rec = sub.add_parser("record", help="record a still test from the board")
    p_rec.add_argument("--label", required=True, help="name for this run, e.g. old or new")
    p_rec.add_argument("--minutes", type=float, default=5.0, help="test length (default 5)")
    p_rec.add_argument("--port", help="serial port (found automatically if omitted)")
    p_rec.set_defaults(func=record)

    p_stats = sub.add_parser("stats", help="print the statistics for a saved CSV")
    p_stats.add_argument("csv")
    p_stats.set_defaults(func=lambda a: print_report(analyze(a.csv)))

    p_cmp = sub.add_parser("compare", help="compare an old-firmware and a new-firmware CSV")
    p_cmp.add_argument("old_csv")
    p_cmp.add_argument("new_csv")
    p_cmp.set_defaults(func=compare)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()

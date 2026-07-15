#!/usr/bin/env python3
"""AirGradient Go thermal-calibration fitter.

Consumes one or more CSVs written by cal_collector.py and automatically
derives, from whatever the data contains:

1. Self-heating model  T_amb = t_dut - k*(x) - b  for every candidate
   internal-temp regressor x in {tdps-t, tfg-t, tdie-t}, using the reference
   board's SHT temperature as ground truth. Reported per segment class
   (all / charging / quiet) with R2 and residual stats.
2. Offset-vs-temperature table: quiet (non-charging, GPS-off) samples binned
   by reference temperature (1 degC bins) — shows whether offsets drift with
   absolute temperature (cold-soak sweeps populate the low bins).
3. Warm-up time constant tau: detects the largest monotonic recovery after a
   cold soak and fits a first-order exponential per temperature signal.

Outputs a console report, <out>/cal_fit.json (coefficients), and
<out>/bins.csv. Plots (<out>/*.png) if matplotlib is installed.

Usage:
    pip install pandas numpy
    python3 cal_fit.py run1.csv run2.csv --out report/
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

import numpy as np
import pandas as pd

RESAMPLE = "10s"
MERGE_TOLERANCE = pd.Timedelta("20s")
CHARGING_STATES = {"trickle", "pre", "fast", "taper", "topoff"}
REGRESSORS = ("tdps", "tfg", "tdie")
MIN_SOAK_RANGE_C = 5.0  # smallest T swing treated as a soak transient
BIN_WIDTH_C = 1.0


def load_runs(paths: list[Path]) -> pd.DataFrame:
    frames = []
    for p in paths:
        df = pd.read_csv(p)
        df["host_ts"] = pd.to_datetime(df["host_ts"], format="ISO8601")
        frames.append(df)
    out = pd.concat(frames).sort_values("host_ts").reset_index(drop=True)
    return out


def per_role(df: pd.DataFrame, role: str) -> pd.DataFrame:
    """Numeric columns for one role, resampled onto a uniform grid."""
    sub = df[df["role"] == role].set_index("host_ts")
    num = sub.select_dtypes(include="number")
    grid = num.resample(RESAMPLE).mean()
    grid["charging"] = (
        sub["chg"].isin(CHARGING_STATES).resample(RESAMPLE).max().astype(bool))
    return grid


def merged(df: pd.DataFrame) -> pd.DataFrame:
    """DUT and ref joined on the time grid (suffix _d / _r)."""
    dut = per_role(df, "dut")
    ref = per_role(df, "ref")
    m = pd.merge_asof(
        dut, ref, left_index=True, right_index=True,
        tolerance=MERGE_TOLERANCE, suffixes=("_d", "_r"), direction="nearest")
    return m.dropna(subset=["t_d", "t_r"])


def fit_line(x: np.ndarray, y: np.ndarray) -> tuple[float, float, float]:
    """Least-squares y = k*x + b. Returns (k, b, r2)."""
    k, b = np.polyfit(x, y, 1)
    pred = k * x + b
    ss_res = float(np.sum((y - pred) ** 2))
    ss_tot = float(np.sum((y - np.mean(y)) ** 2))
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 0 else float("nan")
    return float(k), float(b), r2


def self_heating_fits(m: pd.DataFrame) -> dict:
    """Fit T_bias = k*(T_int - T_sht) + b for every regressor and segment."""
    out: dict = {}
    segments = {
        "all": pd.Series(True, index=m.index),
        "charging": m["charging_d"],
        "quiet": ~m["charging_d"] & (m.get("gps_d", 0) == 0),
    }
    bias = m["t_d"] - m["t_r"]
    for reg in REGRESSORS:
        col = f"{reg}_d"
        if col not in m or m[col].isna().all():
            continue
        delta = m[col] - m["t_d"]
        for seg_name, mask in segments.items():
            sel = mask & delta.notna() & bias.notna()
            n = int(sel.sum())
            if n < 30:
                continue
            k, b, r2 = fit_line(delta[sel].to_numpy(), bias[sel].to_numpy())
            resid = bias[sel] - (k * delta[sel] + b)
            out.setdefault(reg, {})[seg_name] = {
                "k": round(k, 4), "b": round(b, 3), "r2": round(r2, 4),
                "n": n, "resid_std_c": round(float(resid.std()), 3),
                "resid_p95_c": round(float(resid.abs().quantile(0.95)), 3),
            }
    return out


def temperature_bins(m: pd.DataFrame) -> pd.DataFrame:
    """Quiet-segment offsets binned by reference temperature."""
    quiet = m[~m["charging_d"] & (m.get("gps_d", 0) == 0)].copy()
    if quiet.empty:
        return pd.DataFrame()
    quiet["bin_c"] = (quiet["t_r"] / BIN_WIDTH_C).round() * BIN_WIDTH_C
    agg = quiet.groupby("bin_c").agg(
        n=("t_d", "size"),
        dut_minus_ref_c=("t_d", lambda s: float((s - quiet.loc[s.index, "t_r"]).mean())),
        dut_dT_pcb_c=("tdps_d", lambda s: float((s - quiet.loc[s.index, "t_d"]).mean())),
        ref_dT_pcb_c=("tdps_r", lambda s: float((s - quiet.loc[s.index, "t_r"]).mean())),
        rh_dut=("h_d", "mean"),
        rh_ref=("h_r", "mean"),
    )
    return agg.round(3)


def find_warmup(t: pd.Series) -> pd.Series | None:
    """Largest cold-to-warm recovery segment: global min -> following plateau."""
    if t.dropna().empty or t.max() - t.min() < MIN_SOAK_RANGE_C:
        return None
    start = t.idxmin()
    seg = t.loc[start:].dropna()
    # cut at plateau: first point within 0.3 degC of the segment's final value
    final = seg.iloc[-min(len(seg), 30):].mean()
    reached = seg[(final - seg).abs() < 0.3]
    end = reached.index[0] if not reached.empty else seg.index[-1]
    seg = seg.loc[:end]
    return seg if len(seg) >= 20 else None


def fit_tau(seg: pd.Series) -> dict | None:
    """First-order fit T(t) = T_inf - (T_inf - T0)*exp(-t/tau) via log-linear LS."""
    t_inf = seg.iloc[-min(len(seg), 30):].mean() + 0.1  # keep log argument > 0
    y = (t_inf - seg).clip(lower=1e-3)
    x = (seg.index - seg.index[0]).total_seconds().to_numpy()
    slope, intercept = np.polyfit(x, np.log(y.to_numpy()), 1)
    if slope >= 0:
        return None
    tau = -1.0 / slope
    return {
        "tau_s": round(float(tau), 1),
        "t_start_c": round(float(seg.iloc[0]), 2),
        "t_inf_c": round(float(t_inf), 2),
        "span_s": round(float(x[-1]), 0),
    }


def warmup_taus(m: pd.DataFrame) -> dict:
    out = {}
    for label, col in [("dut_sht", "t_d"), ("dut_dps", "tdps_d"),
                       ("ref_sht", "t_r"), ("ref_dps", "tdps_r")]:
        if col not in m:
            continue
        seg = find_warmup(m[col])
        if seg is None:
            continue
        fit = fit_tau(seg)
        if fit:
            out[label] = fit
    return out


def maybe_plots(m: pd.DataFrame, fits: dict, out_dir: Path) -> list[str]:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return []
    written = []

    fig, ax = plt.subplots(figsize=(12, 6))
    for col, label in [("t_d", "DUT SHT"), ("tdps_d", "DUT DPS"),
                       ("t_r", "ref SHT"), ("tdps_r", "ref DPS")]:
        if col in m:
            ax.plot(m.index, m[col], label=label, linewidth=0.8)
    charging = m["charging_d"].astype(bool)
    ax.fill_between(m.index, *ax.get_ylim(), where=charging, alpha=0.1,
                    label="DUT charging")
    ax.set_ylabel("degC")
    ax.legend()
    ax.set_title("thermal-cal timeseries")
    p = out_dir / "timeseries.png"
    fig.savefig(p, dpi=120, bbox_inches="tight")
    plt.close(fig)
    written.append(p.name)

    best = fits.get("tdps", {}).get("all")
    if best:
        fig, ax = plt.subplots(figsize=(7, 6))
        x = (m["tdps_d"] - m["t_d"]).to_numpy()
        y = (m["t_d"] - m["t_r"]).to_numpy()
        ax.scatter(x, y, s=4, alpha=0.3)
        xs = np.linspace(np.nanmin(x), np.nanmax(x), 10)
        ax.plot(xs, best["k"] * xs + best["b"], "r-",
                label=f"k={best['k']}, b={best['b']}, R2={best['r2']}")
        ax.set_xlabel("tdps - t (degC)")
        ax.set_ylabel("t_dut - t_ref (degC)")
        ax.legend()
        ax.set_title("self-heating fit (tdps regressor)")
        p = out_dir / "fit_tdps.png"
        fig.savefig(p, dpi=120, bbox_inches="tight")
        plt.close(fig)
        written.append(p.name)
    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("csv", nargs="+", type=Path, help="collector CSV file(s)")
    parser.add_argument("--out", type=Path, default=Path("cal_report"),
                        help="report output directory")
    args = parser.parse_args()

    df = load_runs(args.csv)
    roles = set(df["role"].unique())
    if "ref" not in roles or "dut" not in roles:
        print(f"need both roles in the data, got: {sorted(roles)}", file=sys.stderr)
        return 1

    m = merged(df)
    span_h = (m.index[-1] - m.index[0]).total_seconds() / 3600
    print(f"samples: {len(m)} merged rows over {span_h:.1f} h "
          f"(dut charging {int(m['charging_d'].sum())} rows)")

    fits = self_heating_fits(m)
    print("\n== self-heating fits: t_dut - t_ref = k*(T_int - t_dut) + b ==")
    for reg, segs in fits.items():
        for seg_name, f in segs.items():
            print(f"  {reg:>5s} [{seg_name:>8s}]  k={f['k']:+.4f}  b={f['b']:+.3f}  "
                  f"R2={f['r2']:.3f}  resid std={f['resid_std_c']:.3f}C "
                  f"p95={f['resid_p95_c']:.3f}C  (n={f['n']})")
    if not fits:
        print("  (not enough overlapping data)")

    bins = temperature_bins(m)
    if not bins.empty:
        print("\n== quiet offsets by reference temperature (degC bins) ==")
        print(bins.to_string())

    taus = warmup_taus(m)
    if taus:
        print("\n== warm-up time constants ==")
        for label, f in taus.items():
            print(f"  {label:>8s}: tau={f['tau_s']:.0f}s "
                  f"({f['t_start_c']}C -> {f['t_inf_c']}C over {f['span_s']:.0f}s)")

    args.out.mkdir(parents=True, exist_ok=True)
    report = {"n_rows": len(m), "span_h": round(span_h, 2),
              "self_heating": fits, "warmup_tau": taus}
    (args.out / "cal_fit.json").write_text(json.dumps(report, indent=2))
    if not bins.empty:
        bins.to_csv(args.out / "bins.csv")
    plots = maybe_plots(m, fits, args.out)
    print(f"\nreport -> {args.out}/cal_fit.json"
          + (f", bins.csv" if not bins.empty else "")
          + (f", {', '.join(plots)}" if plots else " (no matplotlib, plots skipped)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())

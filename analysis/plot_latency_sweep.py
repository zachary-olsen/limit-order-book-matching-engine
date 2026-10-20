"""Plot a latency sweep produced by analysis/latency_sweep.sh.

    python analysis/plot_latency_sweep.py results/latency-sweep-AAPL -o sweep.png

Reads each <latency_us>.txt report and plots, against latency:
  left:  where the P&L per share comes from (spread captured, mid move after, net)
  right: share of fills on quotes that had already been pulled
"""

import argparse
import re
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd

PATTERNS = {
    "spread": r"Spread captured at fill:\s+([+-][\d.]+)",
    "mid_move": r"Mid move 1 s after fill:\s+([+-][\d.]+)",
    "pnl_per_share": r"P&L marked to mid:\s+\$-?[\d.]+ \((-?[\d.]+)",
    "stale_pct": r"\(([\d.]+)% of fills: quote pulled",
}


def load(run_dir: Path) -> pd.DataFrame:
    rows = []
    for report in run_dir.glob("*.txt"):
        text = report.read_text()
        row = {"latency_us": int(report.stem)}
        for key, pattern in PATTERNS.items():
            row[key] = float(re.search(pattern, text).group(1))
        rows.append(row)
    return pd.DataFrame(rows).sort_values("latency_us")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir", type=Path)
    ap.add_argument("-o", "--output", type=Path, default=Path("latency_sweep.png"))
    args = ap.parse_args()

    df = load(args.run_dir)
    # Log axis: plot 0 us at 0.5 us and label it.
    x = df["latency_us"].clip(lower=0.5)
    ticks = sorted(set(x))
    labels = ["0" if t == 0.5 else (f"{t:g} µs" if t < 1000 else f"{t / 1000:g} ms") for t in ticks]

    fig, (ax_pnl, ax_stale) = plt.subplots(1, 2, figsize=(12, 4.5))
    ax_pnl.plot(x, df["spread"], marker="o", label="spread captured at fill")
    ax_pnl.plot(x, df["mid_move"], marker="o", label="mid move in the 1 s after")
    ax_pnl.plot(x, df["pnl_per_share"], marker="o", lw=2.5, label="P&L per share (marked to close)")
    ax_pnl.axhline(0, color="black", lw=0.5)
    ax_pnl.set_ylabel("cents per share")
    ax_pnl.set_title("Where the P&L comes from")
    ax_pnl.legend()

    ax_stale.plot(x, df["stale_pct"], marker="o", color="C3")
    ax_stale.set_ylabel("% of fills")
    ax_stale.set_title("Fills on quotes already pulled (cancel in flight)")

    for ax in (ax_pnl, ax_stale):
        ax.set_xscale("log")
        ax.set_xticks(ticks, labels)
        ax.minorticks_off()
        ax.set_xlabel("order entry and cancel latency")
        ax.grid(True, alpha=0.3)
    fig.suptitle(f"{args.run_dir.name}: market making vs. latency (no fees, rebates or impact)")
    fig.tight_layout()
    fig.savefig(args.output, dpi=150)
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()

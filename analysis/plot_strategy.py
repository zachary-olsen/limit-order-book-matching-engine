"""Plot a strategy simulation from `lob_strategy` output.

    python analysis/plot_strategy.py results/strategy-AAPL -o strategy.png

Top: the mid price. Middle: inventory. Bottom: P&L marked to mid.
Times are Eastern (ITCH timestamps are nanoseconds since midnight).
"""

import argparse
from pathlib import Path

import matplotlib.dates as mdates
import matplotlib.pyplot as plt
import pandas as pd


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir", type=Path, help="directory containing pnl.csv and fills.csv")
    ap.add_argument("-o", "--output", type=Path, default=Path("strategy.png"))
    args = ap.parse_args()

    pnl = pd.read_csv(args.run_dir / "pnl.csv")
    fills = pd.read_csv(args.run_dir / "fills.csv")
    t = pd.to_datetime(pnl["timestamp_ns"], unit="ns")

    fig, (ax_mid, ax_inv, ax_pnl) = plt.subplots(3, 1, figsize=(10, 8), sharex=True)
    ax_mid.plot(t, pnl["mid"], lw=0.8)
    ax_mid.set_ylabel("mid ($)")

    ax_inv.fill_between(t, pnl["inventory"], step="post", alpha=0.6)
    ax_inv.axhline(0, color="black", lw=0.5)
    ax_inv.set_ylabel("inventory (shares)")

    ax_pnl.plot(t, pnl["pnl"], lw=0.8)
    ax_pnl.axhline(0, color="black", lw=0.5)
    ax_pnl.set_ylabel("P&L marked to mid ($)")
    ax_pnl.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))

    traded = fills["qty"].sum()
    fig.suptitle(
        f"{args.run_dir.name}: {len(fills):,} fills, {traded:,} shares traded, "
        f"final P&L ${pnl['pnl'].iloc[-1]:,.0f} (no fees, rebates or impact)"
    )
    for ax in (ax_mid, ax_inv, ax_pnl):
        ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(args.output, dpi=150)
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()

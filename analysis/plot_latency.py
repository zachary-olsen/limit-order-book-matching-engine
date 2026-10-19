"""Plot latency distributions from `lob_latency --csv` output.

    python analysis/plot_latency.py results/<run>/latency.csv [more.csv ...] -o latency.png

Each CSV row is one histogram bucket: workload, lower_ns, upper_ns, count.
Passing several CSVs (e.g. one per book implementation) overlays them.
The plot is a percentile curve with a log-scaled tail axis, so p99 and p99.9
are as visible as the median.
"""

import argparse
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

TAIL_TICKS = [0.5, 0.9, 0.99, 0.999, 0.9999]


def percentile_curve(df: pd.DataFrame) -> tuple[np.ndarray, np.ndarray]:
    df = df.sort_values("lower_ns")
    cum = df["count"].cumsum().to_numpy() / df["count"].sum()
    return cum, df["upper_ns"].to_numpy()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="+", type=Path)
    ap.add_argument("-o", "--output", type=Path, default=Path("latency.png"))
    args = ap.parse_args()

    fig, ax = plt.subplots(figsize=(8, 5))
    for path in args.csv:
        data = pd.read_csv(path)
        for workload, df in data.groupby("workload"):
            cum, ns = percentile_curve(df)
            # x = number of nines: 0.9 -> 1, 0.99 -> 2, ...
            nines = -np.log10(np.clip(1 - cum, 1e-6, None))
            label = workload if len(args.csv) == 1 else f"{path.parent.name}: {workload}"
            ax.step(nines, ns, where="post", label=label)

    ax.set_xticks([-np.log10(1 - p) for p in TAIL_TICKS])
    ax.set_xticklabels([f"p{p * 100:g}" for p in TAIL_TICKS])
    ax.set_yscale("log")
    ax.set_ylabel("latency (ns)")
    ax.set_xlabel("percentile")
    ax.grid(True, which="both", alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(args.output, dpi=150)
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()

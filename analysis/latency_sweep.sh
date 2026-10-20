#!/usr/bin/env bash
# Runs lob_strategy at a range of order entry / cancel latencies.
#
#   analysis/latency_sweep.sh data/07302019.NASDAQ_ITCH50.gz AAPL
#   python analysis/plot_latency_sweep.py results/latency-sweep-AAPL -o sweep.png
set -euo pipefail

cd "$(dirname "$0")/.."
file=${1:?usage: $0 ITCH_FILE SYMBOL}
symbol=${2:?usage: $0 ITCH_FILE SYMBOL}
out="results/latency-sweep-$symbol"

cmake --build --preset release >/dev/null
mkdir -p "$out"
for us in 0 1 5 20 100 1000 10000 100000; do
  echo "latency ${us} us..."
  ./build/release/lob_strategy "$file" --symbol "$symbol" --latency-us "$us" --out "$out/$us" > "$out/$us.txt"
done
echo "reports in $out"

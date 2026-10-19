#!/usr/bin/env bash
# Repeatable benchmark run: Release build, pinned to one core when possible,
# warm-up plus several runs, results table with CPU model.
#
#   bench/run_bench.sh [ITCH_FILE] [extra lob_latency args...]
#
# Results and the latency histogram CSV are written to results/<timestamp>/.
set -euo pipefail

cd "$(dirname "$0")/.."
cmake --preset release >/dev/null
cmake --build --preset release

out="results/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"

args=(--runs 5 --csv "$out/latency.csv")
if [[ $# -gt 0 && -f "$1" ]]; then
  args+=(--itch "$1")
  shift
fi
args+=("$@")

pin=()
if command -v taskset >/dev/null; then
  pin=(taskset -c "${LOB_BENCH_CPU:-2}")
else
  echo "note: taskset not found (not Linux?); running unpinned" >&2
fi

git rev-parse --short HEAD 2>/dev/null | sed 's/^/commit: /' | tee "$out/summary.txt"
"${pin[@]}" ./build/release/lob_latency "${args[@]}" | tee -a "$out/summary.txt"
"${pin[@]}" ./build/release/lob_bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true \
  --benchmark_out="$out/microbench.json" --benchmark_out_format=json | tee -a "$out/summary.txt"

echo "results in $out"

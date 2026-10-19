// Throughput and per-operation latency harness.
//
//   lob_latency [--itch FILE] [--messages N] [--ops N] [--runs R] [--csv PATH]
//
// Workloads:
//   matching          synthetic limit/market/cancel flow through the matching engine
//   replay-synthetic  synthetic ITCH-style book updates (always available)
//   itch-parse        decoding the first N messages of FILE (from memory)
//   itch-replay       applying those messages' book updates (pre-decoded)
//
// Everything is loaded into memory first, so no workload measures disk or
// decompression, and parsing is measured separately from book updates. Each
// workload gets a warm-up pass, then R timed runs; throughput is the median run
// and latency percentiles come from the run with the median p99.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "lob/flow_generator.hpp"
#include "lob/itch/file.hpp"
#include "lob/latency_histogram.hpp"
#include "lob/matching_engine.hpp"
#include "lob/replay.hpp"
#include "synthetic_itch.hpp"
#include "system_info.hpp"

namespace {

using namespace lob;
using Clock = std::chrono::steady_clock;

struct Options {
  std::string itch_path;
  std::uint64_t messages = 20'000'000;
  std::size_t ops = 5'000'000;
  int runs = 5;
  std::string csv;
};

struct Result {
  std::string name;
  std::uint64_t ops = 0;
  double mops = 0;                         // median run, million ops per second
  std::unique_ptr<LatencyHistogram> hist;  // null for throughput-only workloads
};

[[noreturn]] void usage(const char* argv0) {
  std::fprintf(stderr, "usage: %s [--itch FILE] [--messages N] [--ops N] [--runs R] [--csv PATH]\n", argv0);
  std::exit(2);
}

Options parse_args(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) usage(argv[0]);
      return argv[++i];
    };
    if (arg == "--itch") o.itch_path = value();
    else if (arg == "--messages") o.messages = std::stoull(value());
    else if (arg == "--ops") o.ops = std::stoull(value());
    else if (arg == "--runs") o.runs = std::stoi(value());
    else if (arg == "--csv") o.csv = value();
    else usage(argv[0]);
  }
  if (o.runs < 1) usage(argv[0]);
  return o;
}

double seconds_since(Clock::time_point t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

// Times `apply` over every op on a fresh State per run: first without
// per-op timers (throughput), then with them (latency distribution).
template <class State, class Op, class Apply>
Result measure(const std::string& name, const std::vector<Op>& ops, int runs, Apply apply) {
  Result res{name, ops.size(), 0, nullptr};

  std::vector<double> rates;
  for (int r = -1; r < runs; ++r) {  // r == -1 is the warm-up
    auto state = std::make_unique<State>();
    const auto t0 = Clock::now();
    for (const Op& op : ops) apply(*state, op);
    const double secs = seconds_since(t0);
    if (r >= 0) rates.push_back(static_cast<double>(ops.size()) / secs / 1e6);
  }
  res.mops = median(rates);

  std::vector<std::unique_ptr<LatencyHistogram>> hists;
  for (int r = 0; r < runs; ++r) {
    auto state = std::make_unique<State>();
    auto hist = std::make_unique<LatencyHistogram>();
    for (const Op& op : ops) {
      const auto t0 = Clock::now();
      apply(*state, op);
      const auto t1 = Clock::now();
      hist->record(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
    }
    hists.push_back(std::move(hist));
  }
  std::sort(hists.begin(), hists.end(),
            [](const auto& a, const auto& b) { return a->percentile(0.99) < b->percentile(0.99); });
  res.hist = std::move(hists[hists.size() / 2]);
  return res;
}

std::unique_ptr<LatencyHistogram> timer_overhead() {
  auto h = std::make_unique<LatencyHistogram>();
  for (int i = 0; i < 1'000'000; ++i) {
    const auto t0 = Clock::now();
    const auto t1 = Clock::now();
    h->record(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
  }
  return h;
}

// Collects the book-affecting messages as BookOps.
struct OpCollector : itch::NullHandler {
  std::vector<BookOp>* out;
  explicit OpCollector(std::vector<BookOp>& v) : out(&v) {}
  void on_add_order(const itch::AddOrder& m) { out->push_back(to_op(m)); }
  void on_order_executed(const itch::OrderExecuted& m) { out->push_back(to_op(m)); }
  void on_order_executed_with_price(const itch::OrderExecutedWithPrice& m) { out->push_back(to_op(m)); }
  void on_order_cancel(const itch::OrderCancel& m) { out->push_back(to_op(m)); }
  void on_order_delete(const itch::OrderDelete& m) { out->push_back(to_op(m)); }
  void on_order_replace(const itch::OrderReplace& m) { out->push_back(to_op(m)); }
};

Result measure_parse(const std::vector<std::uint8_t>& bytes, std::uint64_t messages, int runs,
                     std::vector<BookOp>& decoded) {
  std::vector<double> rates;
  for (int r = -1; r < runs; ++r) {
    decoded.clear();
    OpCollector sink(decoded);
    const auto t0 = Clock::now();
    itch::parse_buffer(bytes.data(), bytes.size(), sink);
    const double secs = seconds_since(t0);
    if (r >= 0) rates.push_back(static_cast<double>(messages) / secs / 1e6);
  }
  return Result{"itch-parse", messages, median(rates), nullptr};
}

void print_row(const Result& r) {
  std::printf("%-18s %12llu %10.2f", r.name.c_str(), static_cast<unsigned long long>(r.ops), r.mops);
  if (r.hist) {
    const LatencyHistogram& h = *r.hist;
    std::printf(" %8llu %8llu %8llu %8llu %10llu\n", static_cast<unsigned long long>(h.percentile(0.50)),
                static_cast<unsigned long long>(h.percentile(0.90)), static_cast<unsigned long long>(h.percentile(0.99)),
                static_cast<unsigned long long>(h.percentile(0.999)), static_cast<unsigned long long>(h.max()));
  } else {
    std::printf(" %8s %8s %8s %8s %10s\n", "-", "-", "-", "-", "-");
  }
}

void write_csv(const std::string& path, const std::vector<Result>& results) {
  std::ofstream f(path);
  f << "workload,lower_ns,upper_ns,count\n";
  for (const Result& r : results) {
    if (!r.hist) continue;
    for (std::size_t i = 0; i < LatencyHistogram::kBuckets; ++i) {
      if (const auto c = r.hist->bucket_count(i)) {
        f << r.name << ',' << LatencyHistogram::lower_bound(i) << ',' << LatencyHistogram::upper_bound(i) << ','
          << c << '\n';
      }
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parse_args(argc, argv);
  std::vector<Result> results;

  try {
    {
      FlowGenerator gen(1, benchmark_flow());
      std::vector<FlowOp> flow(opt.ops);
      for (auto& op : flow) op = gen.next();
      struct EngineState {
        MatchingEngine<> engine;
        EventBuffer out;
      };
      results.push_back(measure<EngineState>("matching", flow, opt.runs, [](EngineState& s, const FlowOp& op) {
        s.out.clear();
        apply_op(s.engine, op, s.out);
      }));
    }
    {
      const auto ops = bench::synthetic_book_ops(opt.ops);
      results.push_back(measure<BookSet<>>("replay-synthetic", ops, opt.runs,
                                           [](BookSet<>& b, const BookOp& op) { b.apply(op); }));
    }
    if (!opt.itch_path.empty()) {
      std::uint64_t loaded = 0;
      std::vector<BookOp> decoded;
      {
        std::fprintf(stderr, "loading up to %llu messages from %s...\n",
                     static_cast<unsigned long long>(opt.messages), opt.itch_path.c_str());
        itch::GzReader reader(opt.itch_path);
        const auto bytes = itch::load_messages(reader, opt.messages, loaded);
        results.push_back(measure_parse(bytes, loaded, opt.runs, decoded));
      }
      BookSet<> check;
      std::uint64_t invalid = 0;
      for (const BookOp& op : decoded) invalid += check.apply(op) ? 0 : 1;
      if (invalid != 0) {
        std::fprintf(stderr, "warning: %llu invalid references in the loaded slice\n",
                     static_cast<unsigned long long>(invalid));
      }
      results.push_back(measure<BookSet<>>("itch-replay", decoded, opt.runs,
                                           [](BookSet<>& b, const BookOp& op) { b.apply(op); }));
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }

  const auto overhead = timer_overhead();
  std::printf("CPU:      %s\n", bench::cpu_model().c_str());
  std::printf("Compiler: %s\n", bench::compiler().c_str());
  std::printf("Timer:    steady_clock, overhead p50 %llu ns / p99 %llu ns (included in latencies below)\n",
              static_cast<unsigned long long>(overhead->percentile(0.5)),
              static_cast<unsigned long long>(overhead->percentile(0.99)));
  std::printf("Runs:     %d (+1 warm-up); throughput = median run, latency = run with median p99\n", opt.runs);
#ifndef NDEBUG
  std::printf("WARNING:  assertions are enabled; this is not a Release build, numbers are not meaningful\n");
#endif
  std::printf("\n%-18s %12s %10s %8s %8s %8s %8s %10s\n", "workload", "ops", "Mops/s", "p50 ns", "p90 ns", "p99 ns",
              "p99.9 ns", "max ns");
  for (const Result& r : results) print_row(r);

  if (!opt.csv.empty()) write_csv(opt.csv, results);
  return 0;
}

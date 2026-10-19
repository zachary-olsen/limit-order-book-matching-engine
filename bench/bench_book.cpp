// Google Benchmark microbenchmarks. Each book benchmark is a template so new
// OrderBook implementations are added with one BENCHMARK_TEMPLATE line.

#include <benchmark/benchmark.h>

#include <optional>
#include <random>
#include <vector>

#include "lob/flow_generator.hpp"
#include "lob/itch/parser.hpp"
#include "lob/map_order_book.hpp"
#include "lob/matching_engine.hpp"
#include "lob/replay.hpp"
#include "synthetic_itch.hpp"

namespace {

using namespace lob;

std::vector<FlowOp> make_flow(std::size_t n, std::uint64_t seed) {
  FlowGenerator gen(seed, benchmark_flow());
  std::vector<FlowOp> ops(n);
  for (auto& op : ops) op = gen.next();
  return ops;
}

// Synthetic matching flow: limit / market / cancel mix through the engine.
template <class Book>
void BM_MatchingFlow(benchmark::State& state) {
  const auto ops = make_flow(static_cast<std::size_t>(state.range(0)), 1);
  std::optional<MatchingEngine<Book>> engine;
  EventBuffer out;
  for (auto _ : state) {
    state.PauseTiming();
    engine.emplace();
    state.ResumeTiming();
    for (const FlowOp& op : ops) {
      out.clear();
      apply_op(*engine, op, out);
    }
    benchmark::DoNotOptimize(out.size());
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

// Rest N orders across 100 price levels, then cancel them in random order.
template <class Book>
void BM_AddThenCancel(benchmark::State& state) {
  const auto n = static_cast<std::size_t>(state.range(0));
  std::mt19937_64 rng(3);
  std::vector<Price> prices(n);
  for (auto& p : prices) p = 10'000 + static_cast<Price>(rng() % 100);
  std::vector<OrderId> cancel_order(n);
  for (std::size_t i = 0; i < n; ++i) cancel_order[i] = i + 1;
  std::shuffle(cancel_order.begin(), cancel_order.end(), rng);

  std::optional<Book> book;
  for (auto _ : state) {
    state.PauseTiming();
    book.emplace();
    state.ResumeTiming();
    for (std::size_t i = 0; i < n; ++i) book->add(i + 1, Side::Buy, prices[i], 100);
    for (OrderId id : cancel_order) book->remove(id);
    benchmark::DoNotOptimize(book->order_count());
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(2 * n));
}

// Replay-style book updates (ITCH add/execute/cancel/delete/replace), no parsing.
template <class Book>
void BM_ReplayApply(benchmark::State& state) {
  const auto ops = bench::synthetic_book_ops(static_cast<std::size_t>(state.range(0)));
  std::optional<BookSet<Book>> books;
  for (auto _ : state) {
    state.PauseTiming();
    books.emplace();
    state.ResumeTiming();
    std::size_t bad = 0;
    for (const BookOp& op : ops) bad += books->apply(op) ? 0 : 1;
    if (bad != 0) state.SkipWithError("synthetic stream produced invalid references");
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

// ITCH decoding only: a handler that touches each decoded field cheaply.
void BM_ItchParse(benchmark::State& state) {
  const auto bytes = bench::encode(bench::synthetic_book_ops(static_cast<std::size_t>(state.range(0))));
  struct Sink : itch::NullHandler {
    std::uint64_t acc = 0;
    void on_add_order(const itch::AddOrder& m) { acc += m.ref + m.price; }
    void on_order_executed(const itch::OrderExecuted& m) { acc += m.ref; }
    void on_order_cancel(const itch::OrderCancel& m) { acc += m.ref; }
    void on_order_delete(const itch::OrderDelete& m) { acc += m.ref; }
    void on_order_replace(const itch::OrderReplace& m) { acc += m.new_ref; }
  };
  for (auto _ : state) {
    Sink sink;
    itch::parse_buffer(bytes.data(), bytes.size(), sink);
    benchmark::DoNotOptimize(sink.acc);
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(bytes.size()));
}

BENCHMARK_TEMPLATE(BM_MatchingFlow, MapOrderBook)->Arg(100'000)->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_AddThenCancel, MapOrderBook)->Arg(10'000)->Arg(100'000)->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(BM_ReplayApply, MapOrderBook)->Arg(1'000'000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_ItchParse)->Arg(1'000'000)->Unit(benchmark::kMillisecond);

}  // namespace

// Runs a market-making strategy against one symbol of a replayed ITCH day,
// with exact queue-position fill simulation (see lob/strategy.hpp).
//
//   lob_strategy FILE --symbol AAPL [--size N] [--max-inventory N] [--skew N]
//                [--latency-us N] [--out DIR]

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

#include "lob/itch/file.hpp"
#include "lob/pooled_order_book.hpp"
#include "lob/strategy.hpp"

namespace {

using lob::Side;
using lob::sim::InsideSpreadPolicy;
using lob::sim::SimParams;
using Sim = lob::sim::StrategySim<lob::VectorOrderBook, InsideSpreadPolicy>;

constexpr std::uint64_t kNsPerSec = 1'000'000'000ULL;
constexpr double kPricePerDollar = 10'000.0;  // ITCH prices carry 4 implied decimals

struct Options {
  std::string path;
  std::string symbol;
  std::string out;
  InsideSpreadPolicy policy;
  SimParams params;
};

[[noreturn]] void usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s FILE --symbol TICKER [--size N] [--max-inventory N] [--skew N] [--latency-us N] "
               "[--out DIR]\n"
               "  --size           shares per quote (default 100)\n"
               "  --max-inventory  stop quoting a side past this many shares (default 1000)\n"
               "  --skew           shift quotes one tick per N shares of inventory (default 200)\n"
               "  --latency-us     delay before a new quote is live (default 20)\n"
               "  --out            directory for fills.csv and pnl.csv (default results/strategy-TICKER)\n",
               argv0);
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
    if (arg == "--symbol") o.symbol = value();
    else if (arg == "--size") o.policy.size = static_cast<lob::Qty>(std::stoul(value()));
    else if (arg == "--max-inventory") o.policy.max_inventory = std::stoll(value());
    else if (arg == "--skew") o.policy.skew_shares_per_tick = std::stoll(value());
    else if (arg == "--latency-us") o.params.entry_latency_ns = std::stoull(value()) * 1'000;
    else if (arg == "--out") o.out = value();
    else if (arg == "-h" || arg == "--help") usage(argv[0]);
    else if (o.path.empty() && arg[0] != '-') o.path = arg;
    else usage(argv[0]);
  }
  if (o.path.empty() || o.symbol.empty() || o.policy.size == 0 || o.policy.skew_shares_per_tick <= 0) usage(argv[0]);
  if (o.out.empty()) o.out = "results/strategy-" + o.symbol;
  return o;
}

std::string clock_time(std::uint64_t ns) {
  const std::uint64_t s = ns / kNsPerSec;
  char buf[16];
  std::snprintf(buf, sizeof buf, "%02llu:%02llu:%02llu", static_cast<unsigned long long>(s / 3600),
                static_cast<unsigned long long>(s / 60 % 60), static_cast<unsigned long long>(s % 60));
  return buf;
}

void write_csvs(const Sim& sim, const std::string& dir) {
  std::filesystem::create_directories(dir);
  std::ofstream fills(dir + "/fills.csv");
  fills << "timestamp_ns,time,side,price,qty,mid,mid_after\n";
  for (const auto& f : sim.fills()) {
    fills << f.timestamp << ',' << clock_time(f.timestamp) << ',' << (f.side == Side::Buy ? "buy" : "sell") << ','
          << static_cast<double>(f.price) / kPricePerDollar << ',' << f.qty << ',' << f.mid / kPricePerDollar << ','
          << f.mid_after / kPricePerDollar << '\n';
  }
  std::ofstream pnl(dir + "/pnl.csv");
  pnl << "timestamp_ns,time,mid,inventory,pnl\n";
  for (const auto& s : sim.samples()) {
    pnl << s.timestamp << ',' << clock_time(s.timestamp) << ',' << s.mid / kPricePerDollar << ',' << s.inventory
        << ',' << s.pnl / kPricePerDollar << '\n';
  }
}

void report(const Sim& sim, const Options& opt, double secs) {
  const auto& st = sim.stats();
  const auto& p = opt.policy;

  // Share-weighted averages, in cents per share, signed so positive is good for us.
  double edge = 0, markout = 0, drift = 0, marked_shares = 0, traded = 0;
  for (const auto& f : sim.fills()) {
    const double sign = f.side == Side::Buy ? 1.0 : -1.0;
    const double qty = f.qty;
    traded += qty;
    edge += sign * (f.mid - static_cast<double>(f.price)) * qty;
    if (std::isnan(f.mid_after)) continue;
    markout += sign * (f.mid_after - static_cast<double>(f.price)) * qty;
    drift += sign * (f.mid_after - f.mid) * qty;
    marked_shares += qty;
  }
  auto cents = [](double total, double shares) { return shares > 0 ? total / shares / 100.0 : 0.0; };

  std::printf("Strategy: quote %u shares one tick inside the spread (join the best price when the spread is under\n"
              "          3 ticks), shift quotes 1 tick per %lld shares of inventory, |inventory| <= %lld,\n"
              "          new quotes live after %.0f us\n",
              p.size, static_cast<long long>(p.skew_shares_per_tick), static_cast<long long>(p.max_inventory),
              static_cast<double>(opt.params.entry_latency_ns) / 1'000.0);
  std::printf("Symbol:   %s, regular hours, %.1f s to replay\n\n", opt.symbol.c_str(), secs);
  std::printf("Quotes sent / cancelled:   %llu / %llu\n", static_cast<unsigned long long>(st.quotes_sent),
              static_cast<unsigned long long>(st.quotes_cancelled));
  std::printf("Fill rate:                 %.2f%% of quotes got at least one fill (%llu)\n",
              st.quotes_sent ? 100.0 * static_cast<double>(st.quotes_filled) / static_cast<double>(st.quotes_sent) : 0.0,
              static_cast<unsigned long long>(st.quotes_filled));
  std::printf("Fills:                     %llu (bought %llu, sold %llu shares)\n",
              static_cast<unsigned long long>(st.fills), static_cast<unsigned long long>(st.shares_bought),
              static_cast<unsigned long long>(st.shares_sold));
  std::printf("Max |inventory|:           %lld shares (final %lld)\n", static_cast<long long>(st.max_abs_inventory),
              static_cast<long long>(sim.inventory()));
  std::printf("P&L marked to mid:         $%.2f (%.2f cents per share traded)\n", sim.pnl() / kPricePerDollar,
              traded > 0 ? sim.pnl() / traded / 100.0 : 0.0);
  std::printf("Spread captured at fill:   %+.3f cents/share (fill price vs mid)\n", cents(edge, traded));
  std::printf("Mid move 1 s after fill:   %+.3f cents/share (negative = adverse selection)\n",
              cents(drift, marked_shares));
  std::printf("1 s markout:               %+.3f cents/share (fill price vs mid 1 s later)\n",
              cents(markout, marked_shares));
  std::printf("\nNo fees, rebates or market impact are modelled: this is a simulation exercise, not evidence of a\n"
              "profitable strategy. Fills and P&L samples: %s/{fills,pnl}.csv\n",
              opt.out.c_str());
  if (st.invalid_refs != 0) {
    std::printf("WARNING: %llu invalid references\n", static_cast<unsigned long long>(st.invalid_refs));
  }
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parse_args(argc, argv);
  Sim sim(opt.symbol, opt.policy, opt.params);

  const auto start = std::chrono::steady_clock::now();
  try {
    lob::itch::GzReader reader(opt.path);
    lob::itch::stream_file(reader, sim);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  if (!sim.found_symbol()) {
    std::fprintf(stderr, "error: symbol %s not in file\n", opt.symbol.c_str());
    return 1;
  }
  write_csvs(sim, opt.out);
  report(sim, opt, secs);
  return sim.stats().invalid_refs == 0 ? 0 : 3;
}

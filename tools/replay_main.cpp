// Replays a NASDAQ TotalView-ITCH 5.0 file, rebuilding every symbol's book,
// and reports message statistics and validation results. Exits non-zero on
// any invalid reference, malformed message, or book that locks or crosses
// while its stock is trading (halted or paused stocks may legitimately cross).
//
//   lob_replay FILE [--symbol AAPL] [--interval SECONDS] [--max-messages N] [--depth N]
//              [--crossings-csv PATH] [--book map|pool|pool-flat|vector]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "lob/itch/file.hpp"
#include "lob/pooled_order_book.hpp"
#include "lob/replay.hpp"

namespace {

using lob::Price;
using lob::Side;

constexpr std::uint64_t kNsPerSec = 1'000'000'000ULL;

struct Options {
  std::string path;
  std::string symbol;
  std::uint64_t interval_ns = 60 * kNsPerSec;
  std::uint64_t max_messages = UINT64_MAX;
  std::size_t depth = 5;
  std::string crossings_csv;
  std::string book = "map";
};

[[noreturn]] void usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s FILE [--symbol TICKER] [--interval SECONDS] [--max-messages N] [--depth N]\n"
               "          [--crossings-csv PATH] [--book map|pool|pool-flat|vector]\n"
               "  FILE may be gzip-compressed or raw ITCH 5.0.\n"
               "  --symbol prints a top-of-book snapshot for TICKER every interval (default 60s).\n"
               "  --crossings-csv writes every locked/crossed book seen during market hours.\n"
               "  --book picks the order book implementation (default map).\n",
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
    else if (arg == "--interval") o.interval_ns = std::stoull(value()) * kNsPerSec;
    else if (arg == "--max-messages") o.max_messages = std::stoull(value());
    else if (arg == "--depth") o.depth = std::stoul(value());
    else if (arg == "--crossings-csv") o.crossings_csv = value();
    else if (arg == "--book") o.book = value();
    else if (arg == "-h" || arg == "--help") usage(argv[0]);
    else if (o.path.empty() && arg[0] != '-') o.path = arg;
    else usage(argv[0]);
  }
  if (o.path.empty() || o.interval_ns == 0) usage(argv[0]);
  return o;
}

std::string clock_time(std::uint64_t ns) {
  const std::uint64_t s = ns / kNsPerSec;
  char buf[16];
  std::snprintf(buf, sizeof buf, "%02llu:%02llu:%02llu", static_cast<unsigned long long>(s / 3600),
                static_cast<unsigned long long>(s / 60 % 60), static_cast<unsigned long long>(s % 60));
  return buf;
}

std::string dollars(Price p) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%lld.%04lld", static_cast<long long>(p / 10'000),
                static_cast<long long>(p % 10'000));
  return buf;
}

template <class Book>
class ReplayTool : public lob::Replayer<Book> {
  using Base = lob::Replayer<Book>;

 public:
  explicit ReplayTool(const Options& o) : opt_(o), next_snapshot_(o.interval_ns) {}

  void before_message(const lob::itch::Header& h) {
    if (!opt_.symbol.empty()) {
      while (h.timestamp >= next_snapshot_) {
        snapshot(next_snapshot_);
        next_snapshot_ += opt_.interval_ns;
      }
    }
    Base::before_message(h);
  }

  [[nodiscard]] bool stopped() const { return this->stats().messages >= opt_.max_messages; }

  [[nodiscard]] const Book* tracked_book() {
    if (!locate_) locate_ = this->locate_of(opt_.symbol);
    return locate_ ? this->books().find(*locate_) : nullptr;
  }

  void print_depth() {
    const Book* book = tracked_book();
    if (book == nullptr) {
      std::printf("\n%s: no orders seen\n", opt_.symbol.c_str());
      return;
    }
    std::printf("\nFinal %s book (top %zu levels)\n%14s %8s | %-14s %8s\n", opt_.symbol.c_str(), opt_.depth,
                "bid", "shares", "ask", "shares");
    const auto bids = book->levels(Side::Buy, opt_.depth);
    const auto asks = book->levels(Side::Sell, opt_.depth);
    if (bids.empty() && asks.empty()) std::printf("%14s\n", "(book empty)");
    for (std::size_t i = 0; i < std::max(bids.size(), asks.size()); ++i) {
      const std::string b = i < bids.size() ? dollars(bids[i].price) : "";
      const std::string a = i < asks.size() ? dollars(asks[i].price) : "";
      std::printf("%14s %8s | %-14s %8s\n", b.c_str(), i < bids.size() ? std::to_string(bids[i].total).c_str() : "",
                  a.c_str(), i < asks.size() ? std::to_string(asks[i].total).c_str() : "");
    }
  }

 private:
  void snapshot(std::uint64_t at) {
    const Book* book = tracked_book();
    if (book == nullptr || book->order_count() == 0) return;
    const auto bid = book->levels(Side::Buy, 1);
    const auto ask = book->levels(Side::Sell, 1);
    std::printf("%s %-6s", clock_time(at).c_str(), opt_.symbol.c_str());
    if (!bid.empty()) std::printf("  bid %s x %-6llu", dollars(bid[0].price).c_str(),
                                  static_cast<unsigned long long>(bid[0].total));
    else std::printf("  bid %-21s", "-");
    if (!ask.empty()) std::printf("  ask %s x %-6llu", dollars(ask[0].price).c_str(),
                                  static_cast<unsigned long long>(ask[0].total));
    else std::printf("  ask %-21s", "-");
    if (!bid.empty() && !ask.empty()) std::printf("  spread %s", dollars(ask[0].price - bid[0].price).c_str());
    std::printf("  orders %zu%s\n", book->order_count(), this->market_open() ? "" : "  (closed)");
  }

  Options opt_;
  std::uint64_t next_snapshot_;
  std::optional<std::uint16_t> locate_;
};

const char* state_name(char s) {
  switch (s) {
    case 'T': return "trading";
    case 'H': return "halted";
    case 'P': return "paused";
    case 'Q': return "quotation only";
    default: return "unknown";
  }
}

// Summarises locked/crossed books by trading state and by symbol.
template <class Tool>
void report_crossings(const Tool& tool, const Options& opt) {
  const auto& crossings = tool.crossings();
  if (crossings.empty()) return;

  std::map<char, std::uint64_t> by_state;
  struct PerSymbol {
    std::uint64_t count = 0, first = 0, last = 0;
    std::uint64_t locked = 0;  // bid == ask
    char state = '?';
  };
  std::map<std::uint16_t, PerSymbol> by_symbol;
  for (const lob::Crossing& c : crossings) {
    ++by_state[c.trading_state];
    PerSymbol& s = by_symbol[c.locate];
    if (s.count++ == 0) s.first = c.timestamp;
    s.last = c.timestamp;
    s.locked += c.bid == c.ask ? 1 : 0;
    s.state = c.trading_state;
  }

  std::printf("\nLocked/crossed books in market hours by trading state:\n");
  for (const auto& [state, n] : by_state) {
    std::printf("  %-15s %8llu\n", state_name(state), static_cast<unsigned long long>(n));
  }

  std::vector<std::pair<std::uint16_t, PerSymbol>> top(by_symbol.begin(), by_symbol.end());
  std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
  std::printf("\n%zu symbols affected; top 15:\n  %-8s %7s %7s  %-8s  %-8s  %s\n", top.size(), "symbol", "events",
              "locked", "first", "last", "state at last event");
  for (std::size_t i = 0; i < std::min<std::size_t>(15, top.size()); ++i) {
    const auto& [locate, s] = top[i];
    std::printf("  %-8s %7llu %7llu  %-8s  %-8s  %s\n", tool.symbol_of(locate).c_str(),
                static_cast<unsigned long long>(s.count), static_cast<unsigned long long>(s.locked),
                clock_time(s.first).c_str(), clock_time(s.last).c_str(), state_name(s.state));
  }

  if (!opt.crossings_csv.empty()) {
    std::ofstream f(opt.crossings_csv);
    f << "timestamp_ns,time,symbol,trading_state,bid,ask\n";
    for (const lob::Crossing& c : crossings) {
      f << c.timestamp << ',' << clock_time(c.timestamp) << ',' << tool.symbol_of(c.locate) << ','
        << c.trading_state << ',' << dollars(c.bid) << ',' << dollars(c.ask) << '\n';
    }
    std::printf("\nwrote %zu crossings to %s\n", crossings.size(), opt.crossings_csv.c_str());
  }
}

template <class Book>
int run(const Options& opt) {
  ReplayTool<Book> tool(opt);

  const auto start = std::chrono::steady_clock::now();
  std::uint64_t bytes = 0;
  try {
    lob::itch::GzReader reader(opt.path);
    bytes = lob::itch::stream_file(reader, tool);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  const lob::ReplayStats& s = tool.stats();
  std::printf("\nReplayed %llu messages (%.1f MB uncompressed) in %.2f s: %.2f M msg/s including decompression\n",
              static_cast<unsigned long long>(s.messages), static_cast<double>(bytes) / 1e6, secs,
              static_cast<double>(s.messages) / secs / 1e6);
  std::printf("Symbols: %zu   last timestamp: %s\n\nMessages by type:\n", tool.symbol_count(),
              clock_time(tool.last_timestamp()).c_str());
  for (int t = 0; t < 256; ++t) {
    if (s.by_type[static_cast<std::size_t>(t)] == 0) continue;
    std::printf("  %c %14llu\n", t, static_cast<unsigned long long>(s.by_type[static_cast<std::size_t>(t)]));
  }
  std::printf("\nBook ops applied:            %llu\n", static_cast<unsigned long long>(s.book_ops));
  std::printf("Executions (E+C):            %llu (%llu shares)\n", static_cast<unsigned long long>(s.executions),
              static_cast<unsigned long long>(s.executed_shares));
  std::printf("Invalid references:          %llu\n", static_cast<unsigned long long>(s.invalid_refs));
  std::printf("Crossed/locked in mkt hours: %llu (while trading: %llu; the rest while halted or paused)\n",
              static_cast<unsigned long long>(s.crossed_in_market_hours),
              static_cast<unsigned long long>(s.crossed_while_trading));
  std::printf("Malformed messages:          %llu\n", static_cast<unsigned long long>(s.malformed));

  report_crossings(tool, opt);
  if (!opt.symbol.empty()) tool.print_depth();
  return s.invalid_refs == 0 && s.malformed == 0 && s.crossed_while_trading == 0 ? 0 : 3;
}
}  // namespace

int main(int argc, char** argv) {
  const Options opt = parse_args(argc, argv);
  if (opt.book == "map") return run<lob::MapOrderBook>(opt);
  if (opt.book == "pool") return run<lob::PoolOrderBook>(opt);
  if (opt.book == "pool-flat") return run<lob::PoolFlatOrderBook>(opt);
  if (opt.book == "vector") return run<lob::VectorOrderBook>(opt);
  usage(argv[0]);
}

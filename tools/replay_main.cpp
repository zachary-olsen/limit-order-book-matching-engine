// Replays a NASDAQ TotalView-ITCH 5.0 file, rebuilding every symbol's book,
// and reports message statistics and validation results.
//
//   lob_replay FILE [--symbol AAPL] [--interval SECONDS] [--max-messages N] [--depth N]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <optional>
#include <string>

#include "lob/itch/file.hpp"
#include "lob/replay.hpp"

namespace {

using lob::MapOrderBook;
using lob::Price;
using lob::Side;

constexpr std::uint64_t kNsPerSec = 1'000'000'000ULL;

struct Options {
  std::string path;
  std::string symbol;
  std::uint64_t interval_ns = 60 * kNsPerSec;
  std::uint64_t max_messages = UINT64_MAX;
  std::size_t depth = 5;
};

[[noreturn]] void usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s FILE [--symbol TICKER] [--interval SECONDS] [--max-messages N] [--depth N]\n"
               "  FILE may be gzip-compressed or raw ITCH 5.0.\n"
               "  --symbol prints a top-of-book snapshot for TICKER every interval (default 60s).\n",
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

class ReplayTool : public lob::Replayer<> {
 public:
  explicit ReplayTool(const Options& o) : opt_(o), next_snapshot_(o.interval_ns) {}

  void before_message(const lob::itch::Header& h) {
    if (!opt_.symbol.empty()) {
      while (h.timestamp >= next_snapshot_) {
        snapshot(next_snapshot_);
        next_snapshot_ += opt_.interval_ns;
      }
    }
    Replayer::before_message(h);
  }

  [[nodiscard]] bool stopped() const { return stats().messages >= opt_.max_messages; }

  [[nodiscard]] const MapOrderBook* tracked_book() {
    if (!locate_) locate_ = locate_of(opt_.symbol);
    return locate_ ? books().find(*locate_) : nullptr;
  }

  void print_depth() {
    const MapOrderBook* book = tracked_book();
    if (book == nullptr) {
      std::printf("\n%s: no orders seen\n", opt_.symbol.c_str());
      return;
    }
    std::printf("\nFinal %s book (top %zu levels)\n%14s %8s | %-14s %8s\n", opt_.symbol.c_str(), opt_.depth,
                "bid", "shares", "ask", "shares");
    const auto bids = book->levels(Side::Buy, opt_.depth);
    const auto asks = book->levels(Side::Sell, opt_.depth);
    for (std::size_t i = 0; i < std::max(bids.size(), asks.size()); ++i) {
      const std::string b = i < bids.size() ? dollars(bids[i].price) : "";
      const std::string a = i < asks.size() ? dollars(asks[i].price) : "";
      std::printf("%14s %8s | %-14s %8s\n", b.c_str(), i < bids.size() ? std::to_string(bids[i].total).c_str() : "",
                  a.c_str(), i < asks.size() ? std::to_string(asks[i].total).c_str() : "");
    }
  }

 private:
  void snapshot(std::uint64_t at) {
    const MapOrderBook* book = tracked_book();
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
    std::printf("  orders %zu%s\n", book->order_count(), market_open() ? "" : "  (closed)");
  }

  Options opt_;
  std::uint64_t next_snapshot_;
  std::optional<std::uint16_t> locate_;
};

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parse_args(argc, argv);
  ReplayTool tool(opt);

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
  std::printf("Crossed/locked in mkt hours: %llu\n", static_cast<unsigned long long>(s.crossed_in_market_hours));
  std::printf("Malformed messages:          %llu\n", static_cast<unsigned long long>(s.malformed));

  if (!opt.symbol.empty()) tool.print_depth();
  return s.invalid_refs == 0 && s.malformed == 0 ? 0 : 3;
}

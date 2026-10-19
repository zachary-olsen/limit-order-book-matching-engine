#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "lob/itch/parser.hpp"
#include "lob/map_order_book.hpp"
#include "lob/order_book.hpp"

namespace lob {

// A decoded, book-affecting ITCH event. Replay applies these directly to the
// book: the exchange already did the matching, so the matching engine is not
// involved.
enum class BookOpKind : std::uint8_t { Add, Execute, Cancel, Delete, Replace };

struct BookOp {
  OrderId ref;
  OrderId new_ref;  // Replace only
  Price price;      // Add / Replace
  Qty shares;
  std::uint16_t locate;
  BookOpKind kind;
  Side side;  // Add only; Replace keeps the original order's side
};

inline BookOp to_op(const itch::AddOrder& m) {
  return {m.ref, 0, Price{m.price}, m.shares, m.h.locate, BookOpKind::Add, m.side};
}
inline BookOp to_op(const itch::OrderExecuted& m) {
  return {m.ref, 0, 0, m.shares, m.h.locate, BookOpKind::Execute, Side::Buy};
}
inline BookOp to_op(const itch::OrderExecutedWithPrice& m) {
  return {m.ref, 0, 0, m.shares, m.h.locate, BookOpKind::Execute, Side::Buy};
}
inline BookOp to_op(const itch::OrderCancel& m) {
  return {m.ref, 0, 0, m.shares, m.h.locate, BookOpKind::Cancel, Side::Buy};
}
inline BookOp to_op(const itch::OrderDelete& m) {
  return {m.ref, 0, 0, 0, m.h.locate, BookOpKind::Delete, Side::Buy};
}
inline BookOp to_op(const itch::OrderReplace& m) {
  return {m.old_ref, m.new_ref, Price{m.price}, m.shares, m.h.locate, BookOpKind::Replace, Side::Buy};
}

// One book per stock locate code; locate codes index a vector directly
// instead of hashing ticker strings.
template <OrderBook Book = MapOrderBook>
class BookSet {
 public:
  BookSet() : books_(kMaxLocates) {}

  // Returns false if the op references an order that doesn't exist, has too
  // few shares, or (for adds) reuses a live id.
  bool apply(const BookOp& op) {
    Book& b = book(op.locate);
    switch (op.kind) {
      case BookOpKind::Add: return b.add(op.ref, op.side, op.price, op.shares);
      case BookOpKind::Execute:
      case BookOpKind::Cancel: return b.reduce(op.ref, op.shares);
      case BookOpKind::Delete: return b.remove(op.ref);
      case BookOpKind::Replace: {
        const Order* old = b.find(op.ref);
        if (old == nullptr) return false;
        const Side side = old->side;
        b.remove(op.ref);
        return b.add(op.new_ref, side, op.price, op.shares);
      }
    }
    return false;
  }

  Book& book(std::uint16_t locate) {
    auto& slot = books_[locate];
    if (!slot) slot = std::make_unique<Book>();
    return *slot;
  }
  [[nodiscard]] const Book* find(std::uint16_t locate) const { return books_[locate].get(); }

 private:
  static constexpr std::size_t kMaxLocates = 65536;
  std::vector<std::unique_ptr<Book>> books_;
};

struct ReplayStats {
  std::uint64_t messages = 0;
  std::array<std::uint64_t, 256> by_type{};
  std::uint64_t book_ops = 0;
  std::uint64_t invalid_refs = 0;  // must be zero for a correct replay
  std::uint64_t executions = 0;
  std::uint64_t executed_shares = 0;
  std::uint64_t crossed_in_market_hours = 0;  // add/replace left the book locked or crossed
  std::uint64_t malformed = 0;
};

// ITCH handler that rebuilds every symbol's book and validates each reference.
template <OrderBook Book = MapOrderBook>
class Replayer : public itch::NullHandler {
 public:
  void before_message(const itch::Header& h) {
    ++stats_.messages;
    ++stats_.by_type[static_cast<std::uint8_t>(h.type)];
    last_timestamp_ = h.timestamp;
  }

  void on_system_event(const itch::SystemEvent& m) {
    if (m.event_code == 'Q') market_open_ = true;
    if (m.event_code == 'M') market_open_ = false;
  }
  void on_stock_directory(const itch::StockDirectory& m) {
    std::string name = itch::to_string(m.stock);
    locates_[name] = m.h.locate;
    symbols_[m.h.locate] = std::move(name);
  }
  void on_add_order(const itch::AddOrder& m) { apply(to_op(m), true); }
  void on_order_executed(const itch::OrderExecuted& m) { execute(to_op(m), m.shares); }
  void on_order_executed_with_price(const itch::OrderExecutedWithPrice& m) { execute(to_op(m), m.shares); }
  void on_order_cancel(const itch::OrderCancel& m) { apply(to_op(m), false); }
  void on_order_delete(const itch::OrderDelete& m) { apply(to_op(m), false); }
  void on_order_replace(const itch::OrderReplace& m) { apply(to_op(m), true); }
  void on_malformed(char, std::size_t) { ++stats_.malformed; }

  [[nodiscard]] const ReplayStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const BookSet<Book>& books() const noexcept { return books_; }
  [[nodiscard]] bool market_open() const noexcept { return market_open_; }
  [[nodiscard]] std::uint64_t last_timestamp() const noexcept { return last_timestamp_; }
  [[nodiscard]] std::size_t symbol_count() const noexcept { return symbols_.size(); }

  [[nodiscard]] std::optional<std::uint16_t> locate_of(const std::string& symbol) const {
    auto it = locates_.find(symbol);
    if (it == locates_.end()) return std::nullopt;
    return it->second;
  }
  [[nodiscard]] std::string symbol_of(std::uint16_t locate) const {
    auto it = symbols_.find(locate);
    return it == symbols_.end() ? std::string{} : it->second;
  }

 private:
  void execute(const BookOp& op, Qty shares) {
    ++stats_.executions;
    stats_.executed_shares += shares;
    apply(op, false);
  }

  void apply(const BookOp& op, bool may_cross) {
    ++stats_.book_ops;
    if (!books_.apply(op)) {
      ++stats_.invalid_refs;
    } else if (may_cross && market_open_ && is_crossed(books_.book(op.locate))) {
      ++stats_.crossed_in_market_hours;
    }
  }

  BookSet<Book> books_;
  ReplayStats stats_;
  std::unordered_map<std::uint16_t, std::string> symbols_;
  std::unordered_map<std::string, std::uint16_t> locates_;
  std::uint64_t last_timestamp_ = 0;
  bool market_open_ = false;
};

}  // namespace lob

#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "lob/order_book.hpp"
#include "lob/types.hpp"

namespace lob {

// Baseline book: simple, obviously correct, built from standard containers.
//   price levels  std::map keyed so begin() is always the best price
//   time priority std::list FIFO per level
//   cancels       std::unordered_map from id to the order's list position
// Every later implementation is checked against this one.
class MapOrderBook {
 public:
  using LevelInfo = lob::LevelInfo;

  bool add(OrderId id, Side side, Price price, Qty qty);
  bool reduce(OrderId id, Qty qty);
  bool remove(OrderId id);
  void fill_front(Side side, Qty qty);

  [[nodiscard]] const Order* find(OrderId id) const;
  [[nodiscard]] const Order* front(Side side) const;
  [[nodiscard]] std::optional<Price> best_price(Side side) const;
  [[nodiscard]] std::size_t order_count() const noexcept { return index_.size(); }
  [[nodiscard]] std::size_t level_count(Side side) const noexcept;

  // Best-first view of up to max_levels levels (for snapshots and tests).
  [[nodiscard]] std::vector<LevelInfo> levels(Side side, std::size_t max_levels = SIZE_MAX) const;

  // Checks structural invariants; returns an empty string if they all hold,
  // otherwise a description of the first violation. O(n): for tests only.
  [[nodiscard]] std::string validate() const;

 private:
  struct Level {
    std::list<Order> orders;
    std::uint64_t total = 0;
  };
  using BidLevels = std::map<Price, Level, std::greater<>>;
  using AskLevels = std::map<Price, Level, std::less<>>;
  struct Location {
    std::list<Order>::iterator it;
    Level* level;
  };
  using Index = std::unordered_map<OrderId, Location>;

  template <class Self, class F>
  static decltype(auto) on_side(Self& self, Side side, F&& f) {
    if (side == Side::Buy) return f(self.bids_);
    return f(self.asks_);
  }

  void erase(Index::iterator pos);

  BidLevels bids_;
  AskLevels asks_;
  Index index_;
  Seq next_seq_ = 0;
};

static_assert(OrderBook<MapOrderBook>);

}  // namespace lob

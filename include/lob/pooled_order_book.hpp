#pragma once

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "lob/level_store.hpp"
#include "lob/order_book.hpp"
#include "lob/order_index.hpp"
#include "lob/types.hpp"

namespace lob {

// Order book with pooled storage and intrusive FIFO queues:
//  - Every order lives in one contiguous pool (a vector of nodes recycled
//    through a free list), so after warm-up the hot path never allocates.
//  - Each level's FIFO is a doubly-linked list threaded through the nodes by
//    32-bit pool indices, replacing std::list's separately allocated nodes.
//  - Levels are pooled too, so a node can point at its level by index.
// How prices map to levels (Levels) and ids map to nodes (Index) are template
// parameters, so each data-structure choice can be benchmarked on its own.
template <template <bool> class Levels, class Index>
class PooledOrderBook {
 public:
  using LevelInfo = lob::LevelInfo;

  explicit PooledOrderBook(std::size_t expected_orders = 64) { nodes_.reserve(expected_orders); }

  bool add(OrderId id, Side side, Price price, Qty qty) {
    if (qty == 0) return false;
    auto [slot, inserted] = index_.try_emplace(id);
    if (!inserted) return false;
    const std::uint32_t n = alloc_node();
    *slot = n;

    const std::uint32_t l = on_side(side, [&](auto& levels) {
      auto [level_slot, created] = levels.try_emplace(price);
      if (created) *level_slot = alloc_level(price);
      return *level_slot;
    });

    Node& node = nodes_[n];
    Level& level = levels_[l];
    node.order = Order{id, price, qty, side, next_seq_++};
    node.level = l;
    node.prev = level.tail;
    node.next = kNil;
    if (level.tail != kNil) {
      nodes_[level.tail].next = n;
    } else {
      level.head = n;
    }
    level.tail = n;
    level.total += qty;
    ++level.count;
    return true;
  }

  bool reduce(OrderId id, Qty qty) {
    const std::uint32_t* slot = index_.find(id);
    if (slot == nullptr) return false;
    const std::uint32_t n = *slot;
    Order& order = nodes_[n].order;
    if (qty > order.qty) return false;
    if (qty == order.qty) {
      index_.erase(id);
      erase_node(n);
    } else {
      order.qty -= qty;
      levels_[nodes_[n].level].total -= qty;
    }
    return true;
  }

  bool remove(OrderId id) {
    const std::uint32_t* slot = index_.find(id);
    if (slot == nullptr) return false;
    const std::uint32_t n = *slot;
    index_.erase(id);
    erase_node(n);
    return true;
  }

  void fill_front(Side side, Qty qty) {
    const std::uint32_t l = on_side(side, [](const auto& levels) { return levels.best(); });
    Level& level = levels_[l];
    const std::uint32_t n = level.head;
    Order& order = nodes_[n].order;
    order.qty -= qty;
    level.total -= qty;
    if (order.qty == 0) {
      index_.erase(order.id);
      erase_node(n);
    }
  }

  [[nodiscard]] const Order* find(OrderId id) const {
    const std::uint32_t* slot = index_.find(id);
    return slot == nullptr ? nullptr : &nodes_[*slot].order;
  }

  [[nodiscard]] const Order* front(Side side) const {
    return on_side(side, [this](const auto& levels) -> const Order* {
      return levels.empty() ? nullptr : &nodes_[levels_[levels.best()].head].order;
    });
  }

  [[nodiscard]] std::optional<Price> best_price(Side side) const {
    return on_side(side, [this](const auto& levels) -> std::optional<Price> {
      if (levels.empty()) return std::nullopt;
      return levels_[levels.best()].price;
    });
  }

  [[nodiscard]] std::size_t order_count() const noexcept { return index_.size(); }

  [[nodiscard]] std::size_t level_count(Side side) const noexcept {
    return side == Side::Buy ? bids_.size() : asks_.size();
  }

  [[nodiscard]] std::uint64_t depth_at(Side side, Price price) const {
    return on_side(side, [&](const auto& levels) -> std::uint64_t {
      const std::uint32_t* l = levels.find(price);
      return l == nullptr ? 0 : levels_[*l].total;
    });
  }

  [[nodiscard]] std::vector<LevelInfo> levels(Side side, std::size_t max_levels = SIZE_MAX) const {
    std::vector<LevelInfo> out;
    on_side(side, [&](const auto& levels) {
      levels.for_each([&](Price price, std::uint32_t l) {
        if (out.size() >= max_levels) return false;
        out.push_back({price, levels_[l].total, levels_[l].count});
        return true;
      });
    });
    return out;
  }

  // Checks structural invariants; empty string if they hold. O(n), tests only.
  [[nodiscard]] std::string validate() const {
    std::ostringstream err;
    std::size_t resting = 0;
    auto check_side = [&](const auto& levels, Side side) {
      std::optional<Price> prev_price;
      levels.for_each([&](Price price, std::uint32_t l) {
        const Level& level = levels_[l];
        if (prev_price && (side == Side::Buy ? price >= *prev_price : price <= *prev_price)) {
          err << "levels out of order at " << price;
          return false;
        }
        prev_price = price;
        if (level.price != price || level.count == 0 || level.head == kNil) {
          err << "level " << price << " is empty or mislabelled";
          return false;
        }
        std::uint64_t sum = 0;
        std::size_t count = 0;
        std::uint32_t prev = kNil;
        for (std::uint32_t n = level.head; n != kNil; prev = n, n = nodes_[n].next) {
          const Node& node = nodes_[n];
          const Order& o = node.order;
          if (node.prev != prev || node.level != l) {
            err << "broken links at order " << o.id;
            return false;
          }
          if (o.side != side || o.price != price || o.qty == 0) {
            err << "order " << o.id << " has wrong side, price or zero quantity";
            return false;
          }
          if (prev != kNil && nodes_[prev].order.seq >= o.seq) {
            err << "time priority broken at order " << o.id;
            return false;
          }
          const std::uint32_t* slot = index_.find(o.id);
          if (slot == nullptr || *slot != n) {
            err << "index entry missing or stale for order " << o.id;
            return false;
          }
          sum += o.qty;
          ++count;
        }
        if (level.tail != prev || count != level.count || sum != level.total) {
          err << "level " << price << " tail/count/total disagree with its orders";
          return false;
        }
        resting += count;
        return true;
      });
    };
    check_side(bids_, Side::Buy);
    if (err.tellp() == 0) check_side(asks_, Side::Sell);
    if (err.tellp() == 0 && resting != index_.size()) {
      err << "index holds " << index_.size() << " ids but " << resting << " orders rest";
    }
    return err.str();
  }

 private:
  static constexpr std::uint32_t kNil = UINT32_MAX;

  struct Node {
    Order order;
    std::uint32_t prev;
    std::uint32_t next;  // also links the free list
    std::uint32_t level;
  };
  static_assert(sizeof(Node) <= 64, "an order node should fit in one cache line");

  struct Level {
    Price price;
    std::uint64_t total;
    std::uint32_t head;
    std::uint32_t tail;  // also links the free list
    std::uint32_t count;
  };

  template <class Self, class F>
  static decltype(auto) on_side(Self& self, Side side, F&& f) {
    if (side == Side::Buy) return f(self.bids_);
    return f(self.asks_);
  }
  template <class F>
  decltype(auto) on_side(Side side, F&& f) {
    return on_side(*this, side, std::forward<F>(f));
  }
  template <class F>
  decltype(auto) on_side(Side side, F&& f) const {
    return on_side(*this, side, std::forward<F>(f));
  }

  std::uint32_t alloc_node() {
    if (free_node_ != kNil) {
      const std::uint32_t n = free_node_;
      free_node_ = nodes_[n].next;
      return n;
    }
    nodes_.emplace_back();
    return static_cast<std::uint32_t>(nodes_.size() - 1);
  }

  std::uint32_t alloc_level(Price price) {
    std::uint32_t l;
    if (free_level_ != kNil) {
      l = free_level_;
      free_level_ = levels_[l].tail;
    } else {
      levels_.emplace_back();
      l = static_cast<std::uint32_t>(levels_.size() - 1);
    }
    levels_[l] = Level{price, 0, kNil, kNil, 0};
    return l;
  }

  // Unlinks a node from its level (dropping the level if it empties) and
  // returns it to the pool. The caller has already removed its index entry.
  void erase_node(std::uint32_t n) {
    Node& node = nodes_[n];
    const std::uint32_t l = node.level;
    Level& level = levels_[l];
    if (node.prev != kNil) {
      nodes_[node.prev].next = node.next;
    } else {
      level.head = node.next;
    }
    if (node.next != kNil) {
      nodes_[node.next].prev = node.prev;
    } else {
      level.tail = node.prev;
    }
    level.total -= node.order.qty;
    if (--level.count == 0) {
      on_side(node.order.side, [&](auto& levels) { levels.erase(level.price); });
      level.tail = free_level_;
      free_level_ = l;
    }
    node.next = free_node_;
    free_node_ = n;
  }

  std::vector<Node> nodes_;
  std::vector<Level> levels_;
  std::uint32_t free_node_ = kNil;
  std::uint32_t free_level_ = kNil;
  Levels<true> bids_;
  Levels<false> asks_;
  Index index_;
  Seq next_seq_ = 0;
};

// The optimization steps, each one change on top of the previous:
using PoolOrderBook = PooledOrderBook<MapLevels, StdIndex>;         // pooled nodes + intrusive FIFO
using PoolFlatOrderBook = PooledOrderBook<MapLevels, FlatIndex>;    // + open-addressing id index
using VectorOrderBook = PooledOrderBook<VectorLevels, FlatIndex>;   // + sorted-vector price levels

static_assert(OrderBook<PoolOrderBook>);
static_assert(OrderBook<PoolFlatOrderBook>);
static_assert(OrderBook<VectorOrderBook>);

}  // namespace lob

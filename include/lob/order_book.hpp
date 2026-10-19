#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "lob/types.hpp"

namespace lob {

// The interface every book implementation provides. Both the matching engine
// and ITCH replay are written against it, so implementations can be swapped
// (and benchmarked against each other) without touching either.
//
//   add(id, side, price, qty)  rest a new order at the back of its level;
//                              false if qty == 0 or the id already rests
//   reduce(id, qty)            take qty off an order (ITCH E/C/X), removing it
//                              at zero; false if unknown or qty > remaining
//   remove(id)                 delete an order (cancel / ITCH D); false if unknown
//   fill_front(side, qty)      reduce the oldest order at the best price of
//                              `side`; precondition: it exists and qty <= its qty
//   find(id)                   the resting order, or nullptr
//   front(side)                oldest order at the best price, or nullptr
//   best_price(side)           best price on that side, if any
//
// Pointers returned by find() and front() are valid until the next call that
// modifies the book.
template <class B>
concept OrderBook = requires(B& b, const B& cb, OrderId id, Side side, Price price, Qty qty) {
  { b.add(id, side, price, qty) } -> std::same_as<bool>;
  { b.reduce(id, qty) } -> std::same_as<bool>;
  { b.remove(id) } -> std::same_as<bool>;
  { b.fill_front(side, qty) } -> std::same_as<void>;
  { cb.find(id) } -> std::same_as<const Order*>;
  { cb.front(side) } -> std::same_as<const Order*>;
  { cb.best_price(side) } -> std::same_as<std::optional<Price>>;
  { cb.order_count() } -> std::convertible_to<std::size_t>;
};

// Aggregate view of one price level, for snapshots and tests.
struct LevelInfo {
  Price price;
  std::uint64_t total;
  std::size_t orders;
  friend bool operator==(const LevelInfo&, const LevelInfo&) = default;
};

// Crossed or locked: the best bid is at or above the best ask.
template <OrderBook B>
bool is_crossed(const B& book) {
  const auto bid = book.best_price(Side::Buy);
  const auto ask = book.best_price(Side::Sell);
  return bid && ask && *bid >= *ask;
}

}  // namespace lob

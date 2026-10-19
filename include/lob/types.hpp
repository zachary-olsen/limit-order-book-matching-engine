#pragma once

#include <cstdint>

namespace lob {

// Prices are integer ticks, never floating point. ITCH prices carry 4 implied
// decimals ($123.45 == 1234500) and are kept in that form.
using Price = std::int64_t;
using Qty = std::uint32_t;
using OrderId = std::uint64_t;
using Seq = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

constexpr Side opposite(Side side) noexcept {
  return side == Side::Buy ? Side::Sell : Side::Buy;
}

// True if an incoming order on `side` with limit `limit` can trade against a
// resting order at `resting`.
constexpr bool crosses(Side side, Price limit, Price resting) noexcept {
  return side == Side::Buy ? resting <= limit : resting >= limit;
}

struct Order {
  OrderId id;
  Price price;
  Qty qty;  // remaining quantity
  Side side;
  Seq seq;  // arrival sequence; lower means earlier (time priority)
};

}  // namespace lob

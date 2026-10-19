#pragma once

#include <algorithm>

#include "lob/events.hpp"
#include "lob/map_order_book.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"

namespace lob {

// Price-time priority matching on top of any OrderBook implementation.
//
// Rules:
//  - An incoming buy trades against asks while the best ask <= its limit (sells mirror).
//  - Within a level the oldest order fills first.
//  - Every trade executes at the resting order's price.
//  - A limit order's unfilled remainder rests at its limit price.
//  - A market order trades until filled or the opposite side is empty; the
//    remainder is cancelled, never rested.
//  - Cancels remove by id; empty levels disappear immediately (the book's job).
//
// Validation order: zero quantity, then non-positive limit price, then an id
// that is already resting.
template <OrderBook Book = MapOrderBook>
class MatchingEngine {
 public:
  void submit_limit(OrderId id, Side side, Price price, Qty qty, EventBuffer& out) {
    if (qty == 0) {
      out.rejected(id, RejectReason::ZeroQuantity);
      return;
    }
    if (price <= 0) {
      out.rejected(id, RejectReason::InvalidPrice);
      return;
    }
    if (book_.find(id) != nullptr) {
      out.rejected(id, RejectReason::DuplicateId);
      return;
    }
    out.accepted(id, side, price, qty);
    const Qty left = match(id, side, price, /*is_market=*/false, qty, out);
    if (left > 0) book_.add(id, side, price, left);
  }

  void submit_market(OrderId id, Side side, Qty qty, EventBuffer& out) {
    if (qty == 0) {
      out.rejected(id, RejectReason::ZeroQuantity);
      return;
    }
    if (book_.find(id) != nullptr) {
      out.rejected(id, RejectReason::DuplicateId);
      return;
    }
    out.accepted(id, side, 0, qty);
    const Qty left = match(id, side, 0, /*is_market=*/true, qty, out);
    if (left > 0) out.cancelled(id, side, 0, left);
  }

  void cancel(OrderId id, EventBuffer& out) {
    const Order* order = book_.find(id);
    if (order == nullptr) {
      out.rejected(id, RejectReason::UnknownOrder);
      return;
    }
    out.cancelled(id, order->side, order->price, order->qty);
    book_.remove(id);
  }

  [[nodiscard]] const Book& book() const noexcept { return book_; }

 private:
  // Returns the quantity left unfilled.
  Qty match(OrderId id, Side side, Price limit, bool is_market, Qty qty, EventBuffer& out) {
    const Side contra = opposite(side);
    while (qty > 0) {
      const Order* resting = book_.front(contra);
      if (resting == nullptr) break;
      if (!is_market && !crosses(side, limit, resting->price)) break;
      const Qty fill = std::min(qty, resting->qty);
      out.trade(id, resting->id, side, resting->price, fill);
      qty -= fill;
      book_.fill_front(contra, fill);  // may invalidate `resting`
    }
    return qty;
  }

  Book book_;
};

}  // namespace lob

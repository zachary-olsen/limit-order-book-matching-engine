#pragma once

#include <algorithm>
#include <optional>
#include <vector>

#include "lob/events.hpp"
#include "lob/types.hpp"

namespace lob::testing {

// Deliberately dumb reference: every resting order in one flat vector, and a
// linear scan for the best order on each fill. Shares no code with the real
// engine or books, so agreement between the two is meaningful evidence.
class ReferenceEngine {
 public:
  void submit_limit(OrderId id, Side side, Price price, Qty qty, EventBuffer& out) {
    if (qty == 0) {
      out.rejected(id, RejectReason::ZeroQuantity);
    } else if (price <= 0) {
      out.rejected(id, RejectReason::InvalidPrice);
    } else if (find(id) != orders_.end()) {
      out.rejected(id, RejectReason::DuplicateId);
    } else {
      out.accepted(id, side, price, qty);
      qty = match(id, side, price, false, qty, out);
      if (qty > 0) orders_.push_back(Order{id, price, qty, side, seq_++});
    }
  }

  void submit_market(OrderId id, Side side, Qty qty, EventBuffer& out) {
    if (qty == 0) {
      out.rejected(id, RejectReason::ZeroQuantity);
    } else if (find(id) != orders_.end()) {
      out.rejected(id, RejectReason::DuplicateId);
    } else {
      out.accepted(id, side, 0, qty);
      qty = match(id, side, 0, true, qty, out);
      if (qty > 0) out.cancelled(id, side, 0, qty);
    }
  }

  void cancel(OrderId id, EventBuffer& out) {
    auto it = find(id);
    if (it == orders_.end()) {
      out.rejected(id, RejectReason::UnknownOrder);
      return;
    }
    out.cancelled(id, it->side, it->price, it->qty);
    orders_.erase(it);
  }

  [[nodiscard]] std::size_t order_count() const { return orders_.size(); }

  [[nodiscard]] std::optional<Price> best_price(Side side) const {
    std::optional<Price> best;
    for (const Order& o : orders_) {
      if (o.side != side) continue;
      if (!best || (side == Side::Buy ? o.price > *best : o.price < *best)) best = o.price;
    }
    return best;
  }

 private:
  std::vector<Order>::iterator find(OrderId id) {
    return std::find_if(orders_.begin(), orders_.end(), [id](const Order& o) { return o.id == id; });
  }

  // Higher priority: better price for its side, then earlier arrival.
  static bool before(const Order& a, const Order& b) {
    if (a.price != b.price) return a.side == Side::Buy ? a.price > b.price : a.price < b.price;
    return a.seq < b.seq;
  }

  Qty match(OrderId id, Side side, Price limit, bool is_market, Qty qty, EventBuffer& out) {
    while (qty > 0) {
      auto best = orders_.end();
      for (auto it = orders_.begin(); it != orders_.end(); ++it) {
        if (it->side == side) continue;
        if (best == orders_.end() || before(*it, *best)) best = it;
      }
      if (best == orders_.end()) break;
      if (!is_market) {
        const bool too_expensive = side == Side::Buy ? best->price > limit : best->price < limit;
        if (too_expensive) break;
      }
      const Qty fill = qty < best->qty ? qty : best->qty;
      out.trade(id, best->id, side, best->price, fill);
      qty -= fill;
      best->qty -= fill;
      if (best->qty == 0) orders_.erase(best);
    }
    return qty;
  }

  std::vector<Order> orders_;
  Seq seq_ = 0;
};

}  // namespace lob::testing

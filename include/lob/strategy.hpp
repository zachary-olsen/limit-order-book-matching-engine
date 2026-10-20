#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "lob/itch/parser.hpp"
#include "lob/order_book.hpp"
#include "lob/types.hpp"

// Strategy simulation against a replayed ITCH day.
//
// The strategy's orders cannot be inserted into the historical flow, so they
// are "shadow" orders: tracked beside the real book, never in it. ITCH gives
// every individual order, so queue position is exact: when a quote goes live,
// every order already resting at its price is ahead of it and every later
// order is behind it. A shadow quote is then filled when:
//   1. an order behind it at the same price executes (FIFO: we would have
//      traded first), or
//   2. an order at a worse price on the same side executes (price priority),
//   3. an opposite-side order arrives at a price that reaches the quote (it
//      would have traded with us instead of resting).
// Executions, cancels, deletes and replaces of orders ahead of the quote move
// it up the queue.
//
// Latency applies both ways: a new quote is live `latency_ns` after it is
// sent, and a cancelled quote can still be filled until `latency_ns` after
// the cancel is sent. A side can therefore briefly have two quotes at the
// exchange: the one being cancelled and its replacement.
//
// Assumptions: no market impact (the real flow is unchanged by our fills) and
// no fees or rebates.
namespace lob::sim {

struct Quotes {
  std::optional<Price> bid;
  std::optional<Price> ask;
  Qty size = 0;
};

// Quote one tick inside the spread when there is room (spread of at least
// three ticks), otherwise join the best price. Both quotes shift one tick
// against the inventory for every `skew_shares_per_tick` shares held, so a
// long position quotes lower (sells more readily, buys less readily). A side
// is not quoted if a fill could push |inventory| past `max_inventory`.
struct InsideSpreadPolicy {
  Price tick = 100;  // $0.01 in ITCH price units
  Qty size = 100;
  std::int64_t max_inventory = 1'000;
  std::int64_t skew_shares_per_tick = 200;

  [[nodiscard]] Quotes operator()(Price best_bid, Price best_ask, std::int64_t inventory) const {
    const bool room = best_ask - best_bid >= 3 * tick;
    const Price skew = (inventory / skew_shares_per_tick) * tick;
    // Never cross the market: the strategy only posts passive orders.
    const Price bid = std::min((room ? best_bid + tick : best_bid) - skew, best_ask - tick);
    const Price ask = std::max((room ? best_ask - tick : best_ask) - skew, best_bid + tick);

    Quotes q;
    q.size = size;
    if (bid > 0 && bid < ask && inventory + size <= max_inventory) q.bid = bid;
    if (bid < ask && inventory - size >= -max_inventory) q.ask = ask;
    return q;
  }
};

struct SimParams {
  std::uint64_t latency_ns = 20'000;        // order entry and cancel latency to the exchange
  std::uint64_t markout_ns = 1'000'000'000;  // measure the mid this long after each fill
  std::uint64_t sample_ns = 1'000'000'000;   // P&L / inventory sampling interval
};

struct Fill {
  std::uint64_t timestamp;
  Side side;
  Price price;
  Qty qty;
  bool while_cancelling;  // the quote had already been cancelled; the cancel had not arrived
  double mid;             // mid price just before the message that filled us
  double mid_after;       // mid price markout_ns later (NaN until known)
};

struct PnlSample {
  std::uint64_t timestamp;
  double mid;
  std::int64_t inventory;
  double pnl;  // marked to mid, in price units * shares
};

struct SimStats {
  std::uint64_t quotes_sent = 0;
  std::uint64_t quotes_cancelled = 0;
  std::uint64_t quotes_filled = 0;  // quotes that received at least one fill
  std::uint64_t fills = 0;
  std::uint64_t fills_while_cancelling = 0;
  std::uint64_t shares_bought = 0;
  std::uint64_t shares_sold = 0;
  std::int64_t max_abs_inventory = 0;
  std::uint64_t invalid_refs = 0;
};

// Replays one symbol's book from ITCH and runs `Policy` against it. Messages
// for other symbols are skipped, so a run costs little more than decompression.
template <OrderBook Book, class Policy>
class StrategySim : public itch::NullHandler {
 public:
  StrategySim(std::string symbol, Policy policy, SimParams params = {})
      : symbol_(std::move(symbol)), policy_(std::move(policy)), params_(params) {}

  void before_message(const itch::Header& h) {
    now_ = h.timestamp;
    if (mine(h)) {
      advance(Side::Buy);
      advance(Side::Sell);
    }
  }

  void on_system_event(const itch::SystemEvent& m) {
    if (m.event_code == 'Q') {
      market_open_ = true;
      next_sample_ = now_;
    } else if (m.event_code == 'M') {
      update();
      market_open_ = false;
      for (auto& side : quotes_) {
        stats_.quotes_cancelled += static_cast<std::uint64_t>(
            std::count_if(side.begin(), side.end(), [](const Shadow& q) { return !q.dead_at; }));
        side.clear();
      }
      if (last_mid_) samples_.push_back({now_, *last_mid_, inventory_, pnl()});
    }
  }
  void on_stock_directory(const itch::StockDirectory& m) {
    if (itch::to_string(m.stock) == symbol_) locate_ = m.h.locate;
  }
  void on_stock_trading_action(const itch::StockTradingAction& m) {
    if (!mine(m.h)) return;
    trading_ = m.trading_state == 'T';
    update();
  }
  void on_add_order(const itch::AddOrder& m) {
    if (!mine(m.h)) return;
    added(m.ref, m.side, m.price, m.shares);
    update();
  }
  void on_order_executed(const itch::OrderExecuted& m) {
    if (!mine(m.h)) return;
    executed(m.ref, m.shares);
    update();
  }
  void on_order_executed_with_price(const itch::OrderExecutedWithPrice& m) {
    if (!mine(m.h)) return;
    executed(m.ref, m.shares);
    update();
  }
  void on_order_cancel(const itch::OrderCancel& m) {
    if (!mine(m.h)) return;
    removed(m.ref, m.shares, false);
    update();
  }
  void on_order_delete(const itch::OrderDelete& m) {
    if (!mine(m.h)) return;
    removed(m.ref, 0, true);
    update();
  }
  void on_order_replace(const itch::OrderReplace& m) {
    if (!mine(m.h)) return;
    const Order* old = book_.find(m.old_ref);
    if (old == nullptr) {
      ++stats_.invalid_refs;
      return;
    }
    const Side side = old->side;
    removed(m.old_ref, 0, true);  // the replacement loses priority: it re-enters behind us
    added(m.new_ref, side, m.price, m.shares);
    update();
  }

  [[nodiscard]] const SimStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const std::vector<Fill>& fills() const noexcept { return fills_; }
  [[nodiscard]] const std::vector<PnlSample>& samples() const noexcept { return samples_; }
  [[nodiscard]] const Book& book() const noexcept { return book_; }
  [[nodiscard]] std::int64_t inventory() const noexcept { return inventory_; }
  [[nodiscard]] std::int64_t cash() const noexcept { return cash_; }
  [[nodiscard]] std::optional<double> mid() const noexcept { return last_mid_; }
  [[nodiscard]] double pnl() const noexcept {
    return last_mid_ ? static_cast<double>(cash_) + static_cast<double>(inventory_) * *last_mid_ : 0.0;
  }
  [[nodiscard]] bool found_symbol() const noexcept { return locate_.has_value(); }

  // The live quote on a side that is not being cancelled, if any.
  struct QuoteView {
    Price price;
    Qty remaining;
    std::uint64_t ahead;
  };
  [[nodiscard]] std::optional<QuoteView> live_quote(Side side) const {
    for (const Shadow& q : quotes_[index(side)]) {
      if (q.live && !q.dead_at) return QuoteView{q.price, q.remaining, q.ahead};
    }
    return std::nullopt;
  }
  // Number of quotes at the exchange or in flight on a side (including ones being cancelled).
  [[nodiscard]] std::size_t quotes_outstanding(Side side) const { return quotes_[index(side)].size(); }

 private:
  struct Shadow {
    Price price = 0;
    Qty remaining = 0;
    bool live = false;                   // false while the new order is in flight
    std::uint64_t live_at = 0;           // when the new order reaches the exchange
    std::optional<std::uint64_t> dead_at;  // when our cancel reaches the exchange
    std::uint64_t ahead = 0;             // resting quantity ahead of us at our price
    Seq seq_limit = 0;                   // book orders with seq < seq_limit were there before us
    bool filled = false;
  };

  static std::size_t index(Side side) { return side == Side::Buy ? 0 : 1; }
  [[nodiscard]] bool mine(const itch::Header& h) const { return locate_ && h.locate == *locate_; }

  // Live quotes on a side in priority order: better price first, then earlier.
  // Usually one or two; more only when quotes change faster than the latency.
  template <class F>
  void for_each_live(Side side, F&& f) {
    auto& qs = quotes_[index(side)];
    std::sort(qs.begin(), qs.end(), [side](const Shadow& a, const Shadow& b) {
      if (a.price != b.price) return side == Side::Buy ? a.price > b.price : a.price < b.price;
      return a.live_at < b.live_at;
    });
    for (Shadow& q : qs) {
      if (q.live) f(q);
    }
  }

  void added(OrderId ref, Side side, Price price, Qty shares) {
    if (!book_.add(ref, side, price, shares)) {
      ++stats_.invalid_refs;
      return;
    }
    seq_limit_ = book_.find(ref)->seq + 1;
    // Rule 3: an order that reaches our quotes would have traded with them.
    Qty left = shares;
    for_each_live(opposite(side), [&](Shadow& q) {
      if (crosses(side, price, q.price)) left -= fill(opposite(side), q, left);
    });
  }

  void executed(OrderId ref, Qty shares) {
    const Order* o = book_.find(ref);
    if (o == nullptr || shares > o->qty) {
      ++stats_.invalid_refs;
      return;
    }
    const Order before = *o;
    book_.reduce(ref, shares);

    Qty left = shares;
    for_each_live(before.side, [&](Shadow& q) {
      const bool worse_price = before.side == Side::Buy ? before.price < q.price : before.price > q.price;
      if (before.price == q.price && before.seq < q.seq_limit) {
        q.ahead -= std::min<std::uint64_t>(q.ahead, shares);  // ahead of us: we move up
      } else if (before.price == q.price || worse_price) {
        left -= fill(before.side, q, left);  // rules 1 and 2
      }
    });
    prune(before.side);
  }

  void removed(OrderId ref, Qty shares, bool whole) {
    const Order* o = book_.find(ref);
    if (o == nullptr || (!whole && shares > o->qty)) {
      ++stats_.invalid_refs;
      return;
    }
    const Order before = *o;
    const Qty gone = whole ? before.qty : shares;
    if (whole) {
      book_.remove(ref);
    } else {
      book_.reduce(ref, shares);
    }
    for_each_live(before.side, [&](Shadow& q) {
      if (before.price == q.price && before.seq < q.seq_limit) q.ahead -= std::min<std::uint64_t>(q.ahead, gone);
    });
  }

  Qty fill(Side side, Shadow& q, Qty shares) {
    const Qty qty = std::min(q.remaining, shares);
    if (qty == 0) return 0;
    if (!q.filled) {
      q.filled = true;
      ++stats_.quotes_filled;
    }
    q.remaining -= qty;
    const std::int64_t signed_qty = side == Side::Buy ? std::int64_t{qty} : -std::int64_t{qty};
    inventory_ += signed_qty;
    cash_ -= signed_qty * q.price;
    (side == Side::Buy ? stats_.shares_bought : stats_.shares_sold) += qty;
    stats_.max_abs_inventory = std::max(stats_.max_abs_inventory, std::abs(inventory_));
    ++stats_.fills;
    const bool stale = q.dead_at.has_value();
    if (stale) ++stats_.fills_while_cancelling;
    // last_mid_ is still the mid from before this message: the price we were hit at.
    fills_.push_back({now_, side, q.price, qty, stale, last_mid_.value_or(std::nan("")), std::nan("")});
    return qty;
  }

  // Drops fully filled quotes and quotes whose cancel has arrived.
  void prune(Side side) {
    auto& qs = quotes_[index(side)];
    qs.erase(std::remove_if(qs.begin(), qs.end(),
                            [this](const Shadow& q) { return q.remaining == 0 || (q.dead_at && *q.dead_at <= now_); }),
             qs.end());
  }

  // Applies the passage of time: in-flight orders arrive and join the back of
  // their level; cancels arrive and remove their quotes.
  void advance(Side side) {
    for (Shadow& q : quotes_[index(side)]) {
      if (!q.live && q.live_at <= now_) {
        q.live = true;
        q.ahead = book_.depth_at(side, q.price);
        q.seq_limit = seq_limit_;
      }
    }
    prune(side);
  }

  void refresh_mid() {
    const auto bid = book_.best_price(Side::Buy);
    const auto ask = book_.best_price(Side::Sell);
    if (bid && ask && *bid < *ask) last_mid_ = (static_cast<double>(*bid) + static_cast<double>(*ask)) / 2;
  }

  // After every message for our symbol during market hours: mark fills,
  // sample P&L, requote. Outside market hours nothing is quoted and the mid
  // stays frozen at the close, so P&L is marked to the closing mid.
  void update() {
    if (!market_open_) return;
    refresh_mid();
    while (next_markout_ < fills_.size() && fills_[next_markout_].timestamp + params_.markout_ns <= now_) {
      fills_[next_markout_++].mid_after = last_mid_.value_or(std::nan(""));
    }
    if (last_mid_) {
      for (; next_sample_ <= now_; next_sample_ += params_.sample_ns) {
        samples_.push_back({next_sample_, *last_mid_, inventory_, pnl()});
      }
    }

    Quotes want;
    const auto bid = book_.best_price(Side::Buy);
    const auto ask = book_.best_price(Side::Sell);
    if (trading_ && bid && ask && *bid < *ask) want = policy_(*bid, *ask, inventory_);
    place(Side::Buy, want.bid, want.size);
    place(Side::Sell, want.ask, want.size);
    advance(Side::Buy);  // with zero latency, orders and cancels take effect immediately
    advance(Side::Sell);
  }

  // Moves a side toward one working quote at `price` (none if nullopt).
  void place(Side side, std::optional<Price> price, Qty size) {
    auto& qs = quotes_[index(side)];
    bool keep = false;
    for (Shadow& q : qs) {
      if (q.dead_at) continue;
      if (price && q.price == *price && !keep) {
        keep = true;  // already working at this price: keep our queue position
        continue;
      }
      q.dead_at = now_ + params_.latency_ns;  // send a cancel
      ++stats_.quotes_cancelled;
    }
    if (keep || !price || size == 0) return;
    Shadow q;
    q.price = *price;
    q.remaining = size;
    q.live_at = now_ + params_.latency_ns;
    qs.push_back(q);
    ++stats_.quotes_sent;
  }

  std::string symbol_;
  Policy policy_;
  SimParams params_;
  Book book_;
  std::optional<std::uint16_t> locate_;
  bool market_open_ = false;
  bool trading_ = true;  // until an 'H' message says otherwise
  std::uint64_t now_ = 0;
  Seq seq_limit_ = 0;

  std::array<std::vector<Shadow>, 2> quotes_;  // [buy, sell]
  std::int64_t inventory_ = 0;
  std::int64_t cash_ = 0;  // price units * shares
  std::optional<double> last_mid_;

  SimStats stats_;
  std::vector<Fill> fills_;
  std::size_t next_markout_ = 0;
  std::vector<PnlSample> samples_;
  std::uint64_t next_sample_ = 0;
};

}  // namespace lob::sim

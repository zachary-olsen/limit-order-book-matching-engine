#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
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
// Assumptions: no market impact (the real flow is unchanged by our fills),
// no fees or rebates, cancels take effect instantly, and new quotes become
// live only after `entry_latency_ns`.
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
  std::uint64_t entry_latency_ns = 20'000;     // new quotes go live this long after they are sent
  std::uint64_t markout_ns = 1'000'000'000;    // measure the mid this long after each fill
  std::uint64_t sample_ns = 1'000'000'000;     // P&L / inventory sampling interval
};

struct Fill {
  std::uint64_t timestamp;
  Side side;
  Price price;
  Qty qty;
  double mid;        // mid price just before the message that filled us
  double mid_after;  // mid price markout_ns later (NaN until known)
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

  void before_message(const itch::Header& h) { now_ = h.timestamp; }

  void on_system_event(const itch::SystemEvent& m) {
    if (m.event_code == 'Q') {
      market_open_ = true;
      next_sample_ = now_;
    } else if (m.event_code == 'M') {
      update();
      market_open_ = false;
      place(Side::Buy, std::nullopt, 0);
      place(Side::Sell, std::nullopt, 0);
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

  // Live quote on a side, if any: {price, remaining, quantity ahead in queue}.
  struct QuoteView {
    Price price;
    Qty remaining;
    std::uint64_t ahead;
  };
  [[nodiscard]] std::optional<QuoteView> live_quote(Side side) const {
    const Shadow& q = side == Side::Buy ? bid_ : ask_;
    if (q.state != Shadow::State::Live) return std::nullopt;
    return QuoteView{q.price, q.remaining, q.ahead};
  }

 private:
  struct Shadow {
    enum class State : std::uint8_t { Off, Pending, Live };
    State state = State::Off;
    Price price = 0;
    Qty remaining = 0;
    std::uint64_t ahead = 0;     // resting quantity ahead of us at our price
    Seq seq_limit = 0;           // book orders with seq < seq_limit were there before us
    std::uint64_t live_at = 0;   // when a pending quote becomes live
    bool filled = false;
  };

  [[nodiscard]] bool mine(const itch::Header& h) const { return locate_ && h.locate == *locate_; }
  Shadow& quote(Side side) { return side == Side::Buy ? bid_ : ask_; }

  void added(OrderId ref, Side side, Price price, Qty shares) {
    if (!book_.add(ref, side, price, shares)) {
      ++stats_.invalid_refs;
      return;
    }
    seq_limit_ = book_.find(ref)->seq + 1;
    // Rule 3: an order that reaches our quote would have traded with it.
    const Shadow& contra = quote(opposite(side));
    if (contra.state == Shadow::State::Live && crosses(side, price, contra.price)) fill(opposite(side), shares);
  }

  void executed(OrderId ref, Qty shares) {
    const Order* o = book_.find(ref);
    if (o == nullptr || shares > o->qty) {
      ++stats_.invalid_refs;
      return;
    }
    const Order before = *o;
    book_.reduce(ref, shares);

    Shadow& q = quote(before.side);
    if (q.state != Shadow::State::Live) return;
    const bool worse_price = before.side == Side::Buy ? before.price < q.price : before.price > q.price;
    if (before.price == q.price && before.seq < q.seq_limit) {
      q.ahead -= std::min<std::uint64_t>(q.ahead, shares);  // ahead of us: we move up
    } else if (before.price == q.price || worse_price) {
      fill(before.side, shares);  // rules 1 and 2
    }
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
    Shadow& q = quote(before.side);
    if (q.state == Shadow::State::Live && before.price == q.price && before.seq < q.seq_limit) {
      q.ahead -= std::min<std::uint64_t>(q.ahead, gone);
    }
  }

  void fill(Side side, Qty shares) {
    Shadow& q = quote(side);
    const Qty qty = std::min(q.remaining, shares);
    if (qty == 0) return;
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
    // last_mid_ is still the mid from before this message: the price we were hit at.
    fills_.push_back({now_, side, q.price, qty, last_mid_.value_or(std::nan("")), std::nan("")});
    if (q.remaining == 0) q.state = Shadow::State::Off;
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
    if (market_open_ && last_mid_) {
      for (; next_sample_ <= now_; next_sample_ += params_.sample_ns) {
        samples_.push_back({next_sample_, *last_mid_, inventory_, pnl()});
      }
    }

    Quotes want;
    const auto bid = book_.best_price(Side::Buy);
    const auto ask = book_.best_price(Side::Sell);
    if (market_open_ && trading_ && bid && ask && *bid < *ask) want = policy_(*bid, *ask, inventory_);
    place(Side::Buy, want.bid, want.size);
    place(Side::Sell, want.ask, want.size);
    activate(Side::Buy);
    activate(Side::Sell);
  }

  void place(Side side, std::optional<Price> price, Qty size) {
    Shadow& q = quote(side);
    if (q.state != Shadow::State::Off && price && *price == q.price) return;  // keep our queue position
    if (q.state != Shadow::State::Off) {
      q.state = Shadow::State::Off;
      ++stats_.quotes_cancelled;
    }
    if (!price || size == 0) return;
    q = Shadow{Shadow::State::Pending, *price, size, 0, 0, now_ + params_.entry_latency_ns, false};
    ++stats_.quotes_sent;
  }

  // A pending quote joins the back of its level once its entry latency has passed.
  void activate(Side side) {
    Shadow& q = quote(side);
    if (q.state != Shadow::State::Pending || now_ < q.live_at) return;
    q.state = Shadow::State::Live;
    q.ahead = book_.depth_at(side, q.price);
    q.seq_limit = seq_limit_;
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

  Shadow bid_;
  Shadow ask_;
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

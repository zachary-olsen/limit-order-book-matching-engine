#include <gtest/gtest.h>

#include "lob/itch/encoder.hpp"
#include "lob/pooled_order_book.hpp"
#include "lob/strategy.hpp"

namespace lob::sim {
namespace {

using itch::Encoder;

constexpr std::uint16_t kLoc = 5;
constexpr Price kBid = 1'000'000;  // $100.00
constexpr Price kAsk = 1'000'500;  // $100.05

// Quotes fixed prices whenever the book is two-sided.
struct FixedPolicy {
  std::optional<Price> bid;
  std::optional<Price> ask;
  Qty size = 100;
  Quotes operator()(Price, Price, std::int64_t) const { return {bid, ask, size}; }
};

using Sim = StrategySim<VectorOrderBook, FixedPolicy>;

class StrategySimTest : public ::testing::Test {
 protected:
  // Directory entry and market open; each test adds its own orders.
  void SetUp() override {
    enc_.stock_directory(kLoc, 0, "TEST");
    enc_.system_event(0, 1, 'Q');
  }
  void add(OrderId ref, Side side, Qty shares, Price price) {
    enc_.add_order(kLoc, ++ts_, ref, side, shares, "TEST", static_cast<std::uint32_t>(price));
  }
  void execute(OrderId ref, Qty shares) {
    ++ts_;
    enc_.order_executed(kLoc, ts_, ref, shares, /*match=*/ts_);
  }
  Sim run(FixedPolicy policy, SimParams params = no_latency()) {
    Sim sim("TEST", policy, params);
    const auto& b = enc_.bytes();
    EXPECT_EQ(itch::parse_buffer(b.data(), b.size(), sim), b.size());
    EXPECT_EQ(sim.stats().invalid_refs, 0u);
    return sim;
  }
  static SimParams no_latency() {
    SimParams p;
    p.latency_ns = 0;
    return p;
  }

  Encoder enc_;
  std::uint64_t ts_ = 10;
};

TEST_F(StrategySimTest, JoinsBehindRestingOrdersAndMovesUpExactly) {
  add(1, Side::Buy, 300, kBid);
  add(2, Side::Buy, 200, kBid);
  add(3, Side::Sell, 100, kAsk);  // book is two-sided: our bid joins behind 500 shares
  add(4, Side::Buy, 400, kBid);   // arrives after us: behind
  execute(1, 300);                // ahead of us: 500 -> 200
  enc_.order_cancel(kLoc, ++ts_, 2, 50);  // ahead: 200 -> 150
  Sim before_fill = run({kBid, std::nullopt});
  ASSERT_TRUE(before_fill.live_quote(Side::Buy));
  EXPECT_EQ(before_fill.live_quote(Side::Buy)->ahead, 150u);
  EXPECT_EQ(before_fill.stats().fills, 0u);

  enc_.order_delete(kLoc, ++ts_, 2);  // ahead: 150 -> 0
  execute(4, 60);                     // order 4 is behind us, so we would have traded first
  Sim sim = run({kBid, std::nullopt});
  ASSERT_EQ(sim.fills().size(), 1u);
  EXPECT_EQ(sim.fills()[0].side, Side::Buy);
  EXPECT_EQ(sim.fills()[0].price, kBid);
  EXPECT_EQ(sim.fills()[0].qty, 60u);
  EXPECT_EQ(sim.inventory(), 60);
  EXPECT_EQ(sim.cash(), -60 * kBid);
  EXPECT_EQ(sim.live_quote(Side::Buy)->remaining, 40u);
}

TEST_F(StrategySimTest, ExecutionAtWorsePriceFillsBetterQuote) {
  add(1, Side::Buy, 500, kBid);
  add(2, Side::Sell, 100, kAsk);
  execute(1, 150);  // a seller hit $100.00 while we bid $100.01
  Sim sim = run({kBid + 100, std::nullopt});
  EXPECT_EQ(sim.stats().shares_bought, 100u);
  EXPECT_EQ(sim.inventory(), 100);
  EXPECT_EQ(sim.stats().quotes_filled, 1u);
}

TEST_F(StrategySimTest, ExecutionAtBetterPriceDoesNotFill) {
  add(1, Side::Buy, 500, kBid + 100);
  add(2, Side::Sell, 100, kAsk);
  execute(1, 500);  // the order at $100.01 has priority over our $100.00 bid
  Sim sim = run({kBid, std::nullopt});
  EXPECT_EQ(sim.stats().fills, 0u);
}

TEST_F(StrategySimTest, ArrivingOrderThatReachesQuoteFillsIt) {
  add(1, Side::Buy, 100, kBid);
  add(2, Side::Sell, 100, kAsk);
  add(3, Side::Buy, 30, kAsk - 100);  // rests at $100.04, where we offer
  Sim sim = run({std::nullopt, kAsk - 100});
  EXPECT_EQ(sim.stats().shares_sold, 30u);
  EXPECT_EQ(sim.inventory(), -30);
}

TEST_F(StrategySimTest, ReplacedOrderLosesItsPlaceAheadOfUs) {
  add(1, Side::Buy, 200, kBid);
  add(2, Side::Sell, 100, kAsk);  // we join behind order 1
  enc_.order_replace(kLoc, ++ts_, 1, 7, 200, static_cast<std::uint32_t>(kBid));  // now behind us
  execute(7, 50);
  Sim sim = run({kBid, std::nullopt});
  EXPECT_EQ(sim.stats().shares_bought, 50u);
}

TEST_F(StrategySimTest, EntryLatencyStopsRefillDuringOneSweep) {
  add(1, Side::Buy, 1'000, kBid);
  add(2, Side::Sell, 100, kAsk);
  // One aggressive sell prints as several executions with the same timestamp.
  ts_ += 100'000;
  enc_.order_executed(kLoc, ts_, 1, 300, 1);
  enc_.order_executed(kLoc, ts_, 1, 300, 2);
  SimParams p;
  p.latency_ns = 1'000;
  Sim sim = run({kBid + 100, std::nullopt}, p);
  EXPECT_EQ(sim.stats().shares_bought, 100u) << "the refilled quote is not live until 1us later";
  EXPECT_EQ(sim.stats().quotes_sent, 2u);
}

TEST_F(StrategySimTest, PulledQuoteCanBeFilledUntilCancelArrives) {
  add(1, Side::Buy, 1'000, kBid);
  add(2, Side::Sell, 100, kAsk);  // ts 12: our $100.01 bid is sent, live at ts 1012
  ts_ = 2'000;
  add(3, Side::Buy, 100, kBid - 100);  // any message after 1012: the bid is now live
  enc_.stock_trading_action(kLoc, 3'000, "TEST", 'H');  // halt: we pull the bid, cancel lands at 4000
  enc_.order_executed(kLoc, 3'500, 1, 40, 1);  // a seller hits $100.00 before our cancel lands
  enc_.order_executed(kLoc, 5'000, 1, 40, 2);  // after the cancel landed
  SimParams p;
  p.latency_ns = 1'000;
  Sim sim = run({kBid + 100, std::nullopt}, p);
  EXPECT_EQ(sim.stats().shares_bought, 40u);
  EXPECT_EQ(sim.stats().fills_while_cancelling, 1u);
  ASSERT_EQ(sim.fills().size(), 1u);
  EXPECT_TRUE(sim.fills()[0].while_cancelling);
  EXPECT_EQ(sim.quotes_outstanding(Side::Buy), 0u);
}

// Bids at the current best bid, so a new best bid means cancel-and-replace.
struct FollowBestBid {
  Quotes operator()(Price best_bid, Price, std::int64_t) const { return {best_bid, std::nullopt, 100}; }
};

TEST(StrategySimLatency, OldQuoteStaysLiveWhileReplacementIsInFlight) {
  SimParams p;
  p.latency_ns = 1'000;
  StrategySim<VectorOrderBook, FollowBestBid> sim("TEST", FollowBestBid{}, p);
  auto feed = [&sim](const Encoder& e) { itch::parse_buffer(e.bytes().data(), e.bytes().size(), sim); };

  Encoder setup;
  setup.stock_directory(kLoc, 0, "TEST");
  setup.system_event(0, 1, 'Q');
  setup.add_order(kLoc, 10, 1, Side::Buy, 500, "TEST", kBid);
  setup.add_order(kLoc, 11, 2, Side::Sell, 500, "TEST", kAsk);  // quote A sent at kBid, live at 1011
  setup.add_order(kLoc, 2'000, 3, Side::Buy, 100, "TEST", kBid + 100);  // A goes live; then cancel A, send B
  feed(setup);
  EXPECT_EQ(sim.quotes_outstanding(Side::Buy), 2u) << "A awaiting its cancel, B in flight";
  EXPECT_FALSE(sim.live_quote(Side::Buy)) << "A is being cancelled and B is not live yet";

  Encoder later;
  later.add_order(kLoc, 3'500, 4, Side::Sell, 100, "TEST", kAsk + 100);  // both cancel and B have arrived
  feed(later);
  EXPECT_EQ(sim.quotes_outstanding(Side::Buy), 1u);
  ASSERT_TRUE(sim.live_quote(Side::Buy));
  EXPECT_EQ(sim.live_quote(Side::Buy)->price, kBid + 100);
  EXPECT_EQ(sim.live_quote(Side::Buy)->ahead, 100u) << "B joined behind order 3";
}

TEST_F(StrategySimTest, RoundTripPnl) {
  add(1, Side::Buy, 1'000, kBid);
  add(2, Side::Sell, 1'000, kAsk);
  execute(1, 100);  // fills our $100.01 bid
  execute(2, 100);  // fills our $100.03 offer
  Sim sim = run({kBid + 100, kAsk - 200});
  EXPECT_EQ(sim.inventory(), 0);
  EXPECT_EQ(sim.cash(), 100 * 200);  // bought at 100.01, sold at 100.03: $2.00
  EXPECT_DOUBLE_EQ(sim.pnl(), 100.0 * 200);
  EXPECT_EQ(sim.stats().max_abs_inventory, 100);
}

TEST_F(StrategySimTest, QuotesOnlyWhileMarketOpenAndTrading) {
  Encoder pre;
  pre.stock_directory(kLoc, 0, "TEST");
  pre.add_order(kLoc, 2, 1, Side::Buy, 100, "TEST", kBid);
  pre.add_order(kLoc, 3, 2, Side::Sell, 100, "TEST", kAsk);
  Sim sim("TEST", FixedPolicy{kBid, kAsk}, no_latency());
  itch::parse_buffer(pre.bytes().data(), pre.bytes().size(), sim);
  EXPECT_FALSE(sim.live_quote(Side::Buy)) << "before the market opens";

  Encoder open;
  open.system_event(0, 4, 'Q');
  open.add_order(kLoc, 5, 3, Side::Buy, 100, "TEST", kBid - 100);
  itch::parse_buffer(open.bytes().data(), open.bytes().size(), sim);
  EXPECT_TRUE(sim.live_quote(Side::Buy));

  Encoder halt;
  halt.stock_trading_action(kLoc, 6, "TEST", 'H');
  itch::parse_buffer(halt.bytes().data(), halt.bytes().size(), sim);
  EXPECT_FALSE(sim.live_quote(Side::Buy)) << "halted";
}

TEST_F(StrategySimTest, MarksFillsAgainstLaterMid) {
  add(1, Side::Buy, 1'000, kBid);
  add(2, Side::Sell, 1'000, kAsk);
  execute(1, 100);
  ts_ += 2'000'000'000;  // two seconds later the market has moved up a dime
  add(3, Side::Buy, 100, kBid + 1'000);
  add(4, Side::Sell, 100, kAsk + 1'000);
  Sim sim = run({kBid + 100, std::nullopt});
  ASSERT_GE(sim.fills().size(), 1u);
  EXPECT_DOUBLE_EQ(sim.fills()[0].mid, (kBid + kAsk) / 2.0);
  EXPECT_FALSE(std::isnan(sim.fills()[0].mid_after));
}

TEST_F(StrategySimTest, MarksToTheClosingMid) {
  add(1, Side::Buy, 1'000, kBid);
  add(2, Side::Sell, 1'000, kAsk);
  execute(1, 100);  // long 100 at $100.01
  enc_.system_event(0, ++ts_, 'M');
  add(3, Side::Buy, 100, kBid + 50'000);  // after-hours jump must not move the mark
  add(4, Side::Sell, 100, kAsk + 50'000);
  Sim sim = run({kBid + 100, std::nullopt});
  EXPECT_EQ(sim.inventory(), 100);
  EXPECT_DOUBLE_EQ(*sim.mid(), (kBid + kAsk) / 2.0);
  EXPECT_DOUBLE_EQ(sim.pnl(), 100 * ((kBid + kAsk) / 2.0 - (kBid + 100)));
  EXPECT_DOUBLE_EQ(sim.samples().back().pnl, sim.pnl());
}

TEST(InsideSpreadPolicy, QuotesInsideWhenThereIsRoom) {
  const InsideSpreadPolicy p;
  const Quotes q = p(1'000'000, 1'000'500, 0);
  EXPECT_EQ(q.bid, 1'000'100);
  EXPECT_EQ(q.ask, 1'000'400);
  EXPECT_EQ(q.size, 100u);
}

TEST(InsideSpreadPolicy, JoinsBestWhenSpreadIsTight) {
  const InsideSpreadPolicy p;
  const Quotes one_tick = p(1'000'000, 1'000'100, 0);
  EXPECT_EQ(one_tick.bid, 1'000'000);
  EXPECT_EQ(one_tick.ask, 1'000'100);
  const Quotes two_ticks = p(1'000'000, 1'000'200, 0);
  EXPECT_EQ(two_ticks.bid, 1'000'000);
  EXPECT_EQ(two_ticks.ask, 1'000'200);
}

TEST(InsideSpreadPolicy, SkewsAgainstInventoryWithoutCrossing) {
  const InsideSpreadPolicy p;  // one tick per 200 shares
  const Quotes long_pos = p(1'000'000, 1'000'500, 400);
  EXPECT_EQ(long_pos.bid, 1'000'100 - 200);
  EXPECT_EQ(long_pos.ask, 1'000'400 - 200);
  const Quotes very_short = p(1'000'000, 1'000'500, -1'000 + 100);  // skew up 4 ticks
  EXPECT_EQ(very_short.bid, 1'000'400) << "capped one tick below the best ask";
  EXPECT_EQ(very_short.ask, 1'000'800);
}

TEST(InsideSpreadPolicy, StopsQuotingSideAtInventoryLimit) {
  const InsideSpreadPolicy p;
  EXPECT_FALSE(p(1'000'000, 1'000'500, 1'000).bid);
  EXPECT_TRUE(p(1'000'000, 1'000'500, 1'000).ask);
  EXPECT_FALSE(p(1'000'000, 1'000'500, -1'000).ask);
  EXPECT_TRUE(p(1'000'000, 1'000'500, -1'000).bid);
}

}  // namespace
}  // namespace lob::sim

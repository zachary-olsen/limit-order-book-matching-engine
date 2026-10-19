#include <gtest/gtest.h>

#include <vector>

#include "lob/matching_engine.hpp"

namespace lob {
namespace {

Event accepted(OrderId id, Side side, Price price, Qty qty) {
  return {EventType::Accepted, RejectReason::None, side, id, 0, price, qty};
}
Event trade(OrderId id, OrderId resting, Side side, Price price, Qty qty) {
  return {EventType::Trade, RejectReason::None, side, id, resting, price, qty};
}
Event cancelled(OrderId id, Side side, Price price, Qty qty) {
  return {EventType::Cancelled, RejectReason::None, side, id, 0, price, qty};
}
Event rejected(OrderId id, RejectReason reason) {
  return {EventType::Rejected, reason, Side::Buy, id, 0, 0, 0};
}

class MatchingEngineTest : public ::testing::Test {
 protected:
  std::vector<Event> limit(OrderId id, Side side, Price price, Qty qty) {
    out_.clear();
    engine_.submit_limit(id, side, price, qty, out_);
    return {out_.begin(), out_.end()};
  }
  std::vector<Event> market(OrderId id, Side side, Qty qty) {
    out_.clear();
    engine_.submit_market(id, side, qty, out_);
    return {out_.begin(), out_.end()};
  }
  std::vector<Event> cancel(OrderId id) {
    out_.clear();
    engine_.cancel(id, out_);
    return {out_.begin(), out_.end()};
  }
  const MapOrderBook& book() const { return engine_.book(); }

  void TearDown() override {
    EXPECT_EQ(book().validate(), "");
    EXPECT_FALSE(is_crossed(book()));
  }

  MatchingEngine<> engine_;
  EventBuffer out_;
};

using V = std::vector<Event>;

TEST_F(MatchingEngineTest, OrderIntoEmptyBookRests) {
  EXPECT_EQ(limit(1, Side::Buy, 100, 10), V{accepted(1, Side::Buy, 100, 10)});
  ASSERT_NE(book().find(1), nullptr);
  EXPECT_EQ(book().find(1)->qty, 10u);
  EXPECT_EQ(book().best_price(Side::Buy), 100);
}

TEST_F(MatchingEngineTest, NonCrossingOrdersBothRest) {
  limit(1, Side::Buy, 100, 10);
  EXPECT_EQ(limit(2, Side::Sell, 101, 10), V{accepted(2, Side::Sell, 101, 10)});
  EXPECT_EQ(book().order_count(), 2u);
}

TEST_F(MatchingEngineTest, FullCross) {
  limit(1, Side::Sell, 100, 10);
  EXPECT_EQ(limit(2, Side::Buy, 100, 10),
            (V{accepted(2, Side::Buy, 100, 10), trade(2, 1, Side::Buy, 100, 10)}));
  EXPECT_EQ(book().order_count(), 0u);
  EXPECT_EQ(book().level_count(Side::Sell), 0u);
}

TEST_F(MatchingEngineTest, PartialFillRemainderRests) {
  limit(1, Side::Sell, 100, 4);
  EXPECT_EQ(limit(2, Side::Buy, 100, 10),
            (V{accepted(2, Side::Buy, 100, 10), trade(2, 1, Side::Buy, 100, 4)}));
  ASSERT_NE(book().find(2), nullptr);
  EXPECT_EQ(book().find(2)->qty, 6u);
  EXPECT_EQ(book().best_price(Side::Buy), 100);
  EXPECT_FALSE(book().best_price(Side::Sell));
}

TEST_F(MatchingEngineTest, PartialFillOfRestingOrderLeavesItAtFront) {
  limit(1, Side::Sell, 100, 10);
  limit(2, Side::Sell, 100, 10);
  limit(3, Side::Buy, 100, 4);
  EXPECT_EQ(book().front(Side::Sell)->id, 1u);
  EXPECT_EQ(book().front(Side::Sell)->qty, 6u);
}

TEST_F(MatchingEngineTest, SweepsThreePriceLevels) {
  limit(1, Side::Sell, 101, 5);
  limit(2, Side::Sell, 102, 5);
  limit(3, Side::Sell, 103, 5);
  limit(4, Side::Sell, 104, 5);
  EXPECT_EQ(limit(5, Side::Buy, 103, 12),
            (V{accepted(5, Side::Buy, 103, 12), trade(5, 1, Side::Buy, 101, 5),
               trade(5, 2, Side::Buy, 102, 5), trade(5, 3, Side::Buy, 103, 2)}));
  EXPECT_EQ(book().best_price(Side::Sell), 103);
  EXPECT_EQ(book().find(3)->qty, 3u);
  EXPECT_FALSE(book().best_price(Side::Buy));
}

TEST_F(MatchingEngineTest, SweepStopsAtLimitAndRestsRemainder) {
  limit(1, Side::Buy, 99, 5);
  limit(2, Side::Buy, 98, 5);
  EXPECT_EQ(limit(3, Side::Sell, 99, 8),
            (V{accepted(3, Side::Sell, 99, 8), trade(3, 1, Side::Sell, 99, 5)}));
  EXPECT_EQ(book().best_price(Side::Sell), 99);
  EXPECT_EQ(book().best_price(Side::Buy), 98);
  EXPECT_EQ(book().find(3)->qty, 3u);
}

TEST_F(MatchingEngineTest, SamePriceFillsOldestFirst) {
  limit(1, Side::Buy, 100, 5);
  limit(2, Side::Buy, 100, 5);
  limit(3, Side::Buy, 100, 5);
  EXPECT_EQ(limit(4, Side::Sell, 100, 7),
            (V{accepted(4, Side::Sell, 100, 7), trade(4, 1, Side::Sell, 100, 5),
               trade(4, 2, Side::Sell, 100, 2)}));
  EXPECT_EQ(book().front(Side::Buy)->id, 2u);
}

TEST_F(MatchingEngineTest, BetterPriceBeatsEarlierTime) {
  limit(1, Side::Sell, 101, 5);
  limit(2, Side::Sell, 100, 5);
  EXPECT_EQ(market(3, Side::Buy, 5), (V{accepted(3, Side::Buy, 0, 5), trade(3, 2, Side::Buy, 100, 5)}));
}

TEST_F(MatchingEngineTest, TradesAtRestingPriceWhenIncomingIsMoreAggressive) {
  limit(1, Side::Sell, 100, 5);
  EXPECT_EQ(limit(2, Side::Buy, 105, 5),
            (V{accepted(2, Side::Buy, 105, 5), trade(2, 1, Side::Buy, 100, 5)}));
  limit(3, Side::Buy, 100, 5);
  EXPECT_EQ(limit(4, Side::Sell, 90, 5),
            (V{accepted(4, Side::Sell, 90, 5), trade(4, 3, Side::Sell, 100, 5)}));
}

TEST_F(MatchingEngineTest, CancelFromMiddleOfQueue) {
  limit(1, Side::Buy, 100, 1);
  limit(2, Side::Buy, 100, 2);
  limit(3, Side::Buy, 100, 3);
  EXPECT_EQ(cancel(2), V{cancelled(2, Side::Buy, 100, 2)});
  EXPECT_EQ(book().find(2), nullptr);
  EXPECT_EQ(limit(4, Side::Sell, 100, 4),
            (V{accepted(4, Side::Sell, 100, 4), trade(4, 1, Side::Sell, 100, 1),
               trade(4, 3, Side::Sell, 100, 3)}));
}

TEST_F(MatchingEngineTest, CancelLastOrderRemovesLevel) {
  limit(1, Side::Sell, 100, 1);
  limit(2, Side::Sell, 101, 1);
  cancel(1);
  EXPECT_EQ(book().level_count(Side::Sell), 1u);
  EXPECT_EQ(book().best_price(Side::Sell), 101);
}

TEST_F(MatchingEngineTest, CancelUnknownIdIsRejected) {
  EXPECT_EQ(cancel(42), V{rejected(42, RejectReason::UnknownOrder)});
  limit(1, Side::Buy, 100, 1);
  cancel(1);
  EXPECT_EQ(cancel(1), V{rejected(1, RejectReason::UnknownOrder)}) << "double cancel";
}

TEST_F(MatchingEngineTest, CancelOfFilledOrderIsRejected) {
  limit(1, Side::Buy, 100, 1);
  limit(2, Side::Sell, 100, 1);
  EXPECT_EQ(cancel(1), V{rejected(1, RejectReason::UnknownOrder)});
}

TEST_F(MatchingEngineTest, MarketOrderIntoEmptySideIsCancelled) {
  limit(1, Side::Buy, 100, 1);
  EXPECT_EQ(market(2, Side::Buy, 10), (V{accepted(2, Side::Buy, 0, 10), cancelled(2, Side::Buy, 0, 10)}));
  EXPECT_EQ(book().order_count(), 1u);
}

TEST_F(MatchingEngineTest, MarketOrderRemainderIsCancelledNotRested) {
  limit(1, Side::Sell, 100, 3);
  limit(2, Side::Sell, 200, 3);
  EXPECT_EQ(market(3, Side::Buy, 10),
            (V{accepted(3, Side::Buy, 0, 10), trade(3, 1, Side::Buy, 100, 3), trade(3, 2, Side::Buy, 200, 3),
               cancelled(3, Side::Buy, 0, 4)}));
  EXPECT_EQ(book().order_count(), 0u);
}

TEST_F(MatchingEngineTest, MarketOrderFullyFilled) {
  limit(1, Side::Buy, 100, 10);
  EXPECT_EQ(market(2, Side::Sell, 4), (V{accepted(2, Side::Sell, 0, 4), trade(2, 1, Side::Sell, 100, 4)}));
  EXPECT_EQ(book().find(1)->qty, 6u);
}

TEST_F(MatchingEngineTest, RejectsInvalidOrders) {
  EXPECT_EQ(limit(1, Side::Buy, 100, 0), V{rejected(1, RejectReason::ZeroQuantity)});
  EXPECT_EQ(limit(1, Side::Buy, 0, 5), V{rejected(1, RejectReason::InvalidPrice)});
  EXPECT_EQ(limit(1, Side::Buy, -5, 5), V{rejected(1, RejectReason::InvalidPrice)});
  EXPECT_EQ(market(1, Side::Buy, 0), V{rejected(1, RejectReason::ZeroQuantity)});
  EXPECT_EQ(book().order_count(), 0u);
}

TEST_F(MatchingEngineTest, RejectsIdThatIsAlreadyResting) {
  limit(1, Side::Buy, 100, 5);
  EXPECT_EQ(limit(1, Side::Sell, 200, 5), V{rejected(1, RejectReason::DuplicateId)});
  EXPECT_EQ(market(1, Side::Sell, 5), V{rejected(1, RejectReason::DuplicateId)});
  EXPECT_EQ(book().find(1)->qty, 5u);
}

TEST_F(MatchingEngineTest, IdOfFullyFilledOrderCanBeReused) {
  limit(1, Side::Buy, 100, 5);
  limit(2, Side::Sell, 100, 5);
  EXPECT_EQ(limit(1, Side::Buy, 99, 1), V{accepted(1, Side::Buy, 99, 1)});
}

}  // namespace
}  // namespace lob

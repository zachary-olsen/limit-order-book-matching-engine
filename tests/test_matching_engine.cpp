#include <gtest/gtest.h>

#include <vector>

#include "book_types.hpp"
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

template <class Book>
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
  const Book& book() const { return engine_.book(); }

  void TearDown() override {
    EXPECT_EQ(book().validate(), "");
    EXPECT_FALSE(is_crossed(book()));
  }

  MatchingEngine<Book> engine_;
  EventBuffer out_;
};

using V = std::vector<Event>;

TYPED_TEST_SUITE(MatchingEngineTest, testing::AllBooks, testing::BookNames);

TYPED_TEST(MatchingEngineTest, OrderIntoEmptyBookRests) {
  EXPECT_EQ(this->limit(1, Side::Buy, 100, 10), V{accepted(1, Side::Buy, 100, 10)});
  ASSERT_NE(this->book().find(1), nullptr);
  EXPECT_EQ(this->book().find(1)->qty, 10u);
  EXPECT_EQ(this->book().best_price(Side::Buy), 100);
}

TYPED_TEST(MatchingEngineTest, NonCrossingOrdersBothRest) {
  this->limit(1, Side::Buy, 100, 10);
  EXPECT_EQ(this->limit(2, Side::Sell, 101, 10), V{accepted(2, Side::Sell, 101, 10)});
  EXPECT_EQ(this->book().order_count(), 2u);
}

TYPED_TEST(MatchingEngineTest, FullCross) {
  this->limit(1, Side::Sell, 100, 10);
  EXPECT_EQ(this->limit(2, Side::Buy, 100, 10),
            (V{accepted(2, Side::Buy, 100, 10), trade(2, 1, Side::Buy, 100, 10)}));
  EXPECT_EQ(this->book().order_count(), 0u);
  EXPECT_EQ(this->book().level_count(Side::Sell), 0u);
}

TYPED_TEST(MatchingEngineTest, PartialFillRemainderRests) {
  this->limit(1, Side::Sell, 100, 4);
  EXPECT_EQ(this->limit(2, Side::Buy, 100, 10),
            (V{accepted(2, Side::Buy, 100, 10), trade(2, 1, Side::Buy, 100, 4)}));
  ASSERT_NE(this->book().find(2), nullptr);
  EXPECT_EQ(this->book().find(2)->qty, 6u);
  EXPECT_EQ(this->book().best_price(Side::Buy), 100);
  EXPECT_FALSE(this->book().best_price(Side::Sell));
}

TYPED_TEST(MatchingEngineTest, PartialFillOfRestingOrderLeavesItAtFront) {
  this->limit(1, Side::Sell, 100, 10);
  this->limit(2, Side::Sell, 100, 10);
  this->limit(3, Side::Buy, 100, 4);
  EXPECT_EQ(this->book().front(Side::Sell)->id, 1u);
  EXPECT_EQ(this->book().front(Side::Sell)->qty, 6u);
}

TYPED_TEST(MatchingEngineTest, SweepsThreePriceLevels) {
  this->limit(1, Side::Sell, 101, 5);
  this->limit(2, Side::Sell, 102, 5);
  this->limit(3, Side::Sell, 103, 5);
  this->limit(4, Side::Sell, 104, 5);
  EXPECT_EQ(this->limit(5, Side::Buy, 103, 12),
            (V{accepted(5, Side::Buy, 103, 12), trade(5, 1, Side::Buy, 101, 5),
               trade(5, 2, Side::Buy, 102, 5), trade(5, 3, Side::Buy, 103, 2)}));
  EXPECT_EQ(this->book().best_price(Side::Sell), 103);
  EXPECT_EQ(this->book().find(3)->qty, 3u);
  EXPECT_FALSE(this->book().best_price(Side::Buy));
}

TYPED_TEST(MatchingEngineTest, SweepStopsAtLimitAndRestsRemainder) {
  this->limit(1, Side::Buy, 99, 5);
  this->limit(2, Side::Buy, 98, 5);
  EXPECT_EQ(this->limit(3, Side::Sell, 99, 8),
            (V{accepted(3, Side::Sell, 99, 8), trade(3, 1, Side::Sell, 99, 5)}));
  EXPECT_EQ(this->book().best_price(Side::Sell), 99);
  EXPECT_EQ(this->book().best_price(Side::Buy), 98);
  EXPECT_EQ(this->book().find(3)->qty, 3u);
}

TYPED_TEST(MatchingEngineTest, SamePriceFillsOldestFirst) {
  this->limit(1, Side::Buy, 100, 5);
  this->limit(2, Side::Buy, 100, 5);
  this->limit(3, Side::Buy, 100, 5);
  EXPECT_EQ(this->limit(4, Side::Sell, 100, 7),
            (V{accepted(4, Side::Sell, 100, 7), trade(4, 1, Side::Sell, 100, 5),
               trade(4, 2, Side::Sell, 100, 2)}));
  EXPECT_EQ(this->book().front(Side::Buy)->id, 2u);
}

TYPED_TEST(MatchingEngineTest, BetterPriceBeatsEarlierTime) {
  this->limit(1, Side::Sell, 101, 5);
  this->limit(2, Side::Sell, 100, 5);
  EXPECT_EQ(this->market(3, Side::Buy, 5), (V{accepted(3, Side::Buy, 0, 5), trade(3, 2, Side::Buy, 100, 5)}));
}

TYPED_TEST(MatchingEngineTest, TradesAtRestingPriceWhenIncomingIsMoreAggressive) {
  this->limit(1, Side::Sell, 100, 5);
  EXPECT_EQ(this->limit(2, Side::Buy, 105, 5),
            (V{accepted(2, Side::Buy, 105, 5), trade(2, 1, Side::Buy, 100, 5)}));
  this->limit(3, Side::Buy, 100, 5);
  EXPECT_EQ(this->limit(4, Side::Sell, 90, 5),
            (V{accepted(4, Side::Sell, 90, 5), trade(4, 3, Side::Sell, 100, 5)}));
}

TYPED_TEST(MatchingEngineTest, CancelFromMiddleOfQueue) {
  this->limit(1, Side::Buy, 100, 1);
  this->limit(2, Side::Buy, 100, 2);
  this->limit(3, Side::Buy, 100, 3);
  EXPECT_EQ(this->cancel(2), V{cancelled(2, Side::Buy, 100, 2)});
  EXPECT_EQ(this->book().find(2), nullptr);
  EXPECT_EQ(this->limit(4, Side::Sell, 100, 4),
            (V{accepted(4, Side::Sell, 100, 4), trade(4, 1, Side::Sell, 100, 1),
               trade(4, 3, Side::Sell, 100, 3)}));
}

TYPED_TEST(MatchingEngineTest, CancelLastOrderRemovesLevel) {
  this->limit(1, Side::Sell, 100, 1);
  this->limit(2, Side::Sell, 101, 1);
  this->cancel(1);
  EXPECT_EQ(this->book().level_count(Side::Sell), 1u);
  EXPECT_EQ(this->book().best_price(Side::Sell), 101);
}

TYPED_TEST(MatchingEngineTest, CancelUnknownIdIsRejected) {
  EXPECT_EQ(this->cancel(42), V{rejected(42, RejectReason::UnknownOrder)});
  this->limit(1, Side::Buy, 100, 1);
  this->cancel(1);
  EXPECT_EQ(this->cancel(1), V{rejected(1, RejectReason::UnknownOrder)}) << "double cancel";
}

TYPED_TEST(MatchingEngineTest, CancelOfFilledOrderIsRejected) {
  this->limit(1, Side::Buy, 100, 1);
  this->limit(2, Side::Sell, 100, 1);
  EXPECT_EQ(this->cancel(1), V{rejected(1, RejectReason::UnknownOrder)});
}

TYPED_TEST(MatchingEngineTest, MarketOrderIntoEmptySideIsCancelled) {
  this->limit(1, Side::Buy, 100, 1);
  EXPECT_EQ(this->market(2, Side::Buy, 10), (V{accepted(2, Side::Buy, 0, 10), cancelled(2, Side::Buy, 0, 10)}));
  EXPECT_EQ(this->book().order_count(), 1u);
}

TYPED_TEST(MatchingEngineTest, MarketOrderRemainderIsCancelledNotRested) {
  this->limit(1, Side::Sell, 100, 3);
  this->limit(2, Side::Sell, 200, 3);
  EXPECT_EQ(this->market(3, Side::Buy, 10),
            (V{accepted(3, Side::Buy, 0, 10), trade(3, 1, Side::Buy, 100, 3), trade(3, 2, Side::Buy, 200, 3),
               cancelled(3, Side::Buy, 0, 4)}));
  EXPECT_EQ(this->book().order_count(), 0u);
}

TYPED_TEST(MatchingEngineTest, MarketOrderFullyFilled) {
  this->limit(1, Side::Buy, 100, 10);
  EXPECT_EQ(this->market(2, Side::Sell, 4), (V{accepted(2, Side::Sell, 0, 4), trade(2, 1, Side::Sell, 100, 4)}));
  EXPECT_EQ(this->book().find(1)->qty, 6u);
}

TYPED_TEST(MatchingEngineTest, RejectsInvalidOrders) {
  EXPECT_EQ(this->limit(1, Side::Buy, 100, 0), V{rejected(1, RejectReason::ZeroQuantity)});
  EXPECT_EQ(this->limit(1, Side::Buy, 0, 5), V{rejected(1, RejectReason::InvalidPrice)});
  EXPECT_EQ(this->limit(1, Side::Buy, -5, 5), V{rejected(1, RejectReason::InvalidPrice)});
  EXPECT_EQ(this->market(1, Side::Buy, 0), V{rejected(1, RejectReason::ZeroQuantity)});
  EXPECT_EQ(this->book().order_count(), 0u);
}

TYPED_TEST(MatchingEngineTest, RejectsIdThatIsAlreadyResting) {
  this->limit(1, Side::Buy, 100, 5);
  EXPECT_EQ(this->limit(1, Side::Sell, 200, 5), V{rejected(1, RejectReason::DuplicateId)});
  EXPECT_EQ(this->market(1, Side::Sell, 5), V{rejected(1, RejectReason::DuplicateId)});
  EXPECT_EQ(this->book().find(1)->qty, 5u);
}

TYPED_TEST(MatchingEngineTest, IdOfFullyFilledOrderCanBeReused) {
  this->limit(1, Side::Buy, 100, 5);
  this->limit(2, Side::Sell, 100, 5);
  EXPECT_EQ(this->limit(1, Side::Buy, 99, 1), V{accepted(1, Side::Buy, 99, 1)});
}

}  // namespace
}  // namespace lob

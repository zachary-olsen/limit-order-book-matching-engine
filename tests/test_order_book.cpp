#include <gtest/gtest.h>

#include "book_types.hpp"

namespace lob {
namespace {

using Level = LevelInfo;

template <class Book>
class OrderBookTest : public ::testing::Test {};
TYPED_TEST_SUITE(OrderBookTest, testing::AllBooks, testing::BookNames);

TYPED_TEST(OrderBookTest, EmptyBook) {
  TypeParam book;
  EXPECT_EQ(book.order_count(), 0u);
  EXPECT_FALSE(book.best_price(Side::Buy));
  EXPECT_FALSE(book.best_price(Side::Sell));
  EXPECT_EQ(book.front(Side::Buy), nullptr);
  EXPECT_FALSE(is_crossed(book));
  EXPECT_EQ(book.validate(), "");
}

TYPED_TEST(OrderBookTest, BestPricesAndLevelOrdering) {
  TypeParam book;
  ASSERT_TRUE(book.add(1, Side::Buy, 100, 10));
  ASSERT_TRUE(book.add(2, Side::Buy, 102, 20));
  ASSERT_TRUE(book.add(3, Side::Buy, 101, 30));
  ASSERT_TRUE(book.add(4, Side::Sell, 105, 5));
  ASSERT_TRUE(book.add(5, Side::Sell, 103, 6));

  EXPECT_EQ(book.best_price(Side::Buy), 102);
  EXPECT_EQ(book.best_price(Side::Sell), 103);
  EXPECT_EQ(book.levels(Side::Buy),
            (std::vector<Level>{{102, 20, 1}, {101, 30, 1}, {100, 10, 1}}));
  EXPECT_EQ(book.levels(Side::Sell, 1), (std::vector<Level>{{103, 6, 1}}));
  EXPECT_EQ(book.depth_at(Side::Buy, 101), 30u);
  EXPECT_EQ(book.depth_at(Side::Sell, 105), 5u);
  EXPECT_EQ(book.depth_at(Side::Sell, 104), 0u);
  EXPECT_EQ(book.depth_at(Side::Buy, 103), 0u) << "price on the other side";
  EXPECT_EQ(book.validate(), "");
}

TYPED_TEST(OrderBookTest, RejectsDuplicateIdAndZeroQuantity) {
  TypeParam book;
  ASSERT_TRUE(book.add(1, Side::Buy, 100, 10));
  EXPECT_FALSE(book.add(1, Side::Sell, 200, 10));
  EXPECT_FALSE(book.add(2, Side::Buy, 100, 0));
  EXPECT_EQ(book.order_count(), 1u);
}

TYPED_TEST(OrderBookTest, ReducePartialThenToZeroRemovesOrderAndLevel) {
  TypeParam book;
  ASSERT_TRUE(book.add(1, Side::Sell, 100, 10));
  EXPECT_TRUE(book.reduce(1, 4));
  EXPECT_EQ(book.find(1)->qty, 6u);
  EXPECT_EQ(book.levels(Side::Sell), (std::vector<Level>{{100, 6, 1}}));
  EXPECT_FALSE(book.reduce(1, 7)) << "cannot take more than remains";
  EXPECT_TRUE(book.reduce(1, 6));
  EXPECT_EQ(book.find(1), nullptr);
  EXPECT_EQ(book.level_count(Side::Sell), 0u);
  EXPECT_FALSE(book.reduce(1, 1));
  EXPECT_EQ(book.validate(), "");
}

TYPED_TEST(OrderBookTest, RemoveFromMiddleKeepsFifoOrder) {
  TypeParam book;
  ASSERT_TRUE(book.add(1, Side::Buy, 100, 1));
  ASSERT_TRUE(book.add(2, Side::Buy, 100, 2));
  ASSERT_TRUE(book.add(3, Side::Buy, 100, 3));
  EXPECT_TRUE(book.remove(2));
  EXPECT_FALSE(book.remove(2));
  EXPECT_EQ(book.levels(Side::Buy), (std::vector<Level>{{100, 4, 2}}));
  EXPECT_EQ(book.front(Side::Buy)->id, 1u);
  book.fill_front(Side::Buy, 1);
  EXPECT_EQ(book.front(Side::Buy)->id, 3u);
  EXPECT_EQ(book.validate(), "");
}

TYPED_TEST(OrderBookTest, FillFrontEmptiesLevelsInPriceOrder) {
  TypeParam book;
  ASSERT_TRUE(book.add(1, Side::Sell, 101, 5));
  ASSERT_TRUE(book.add(2, Side::Sell, 100, 5));
  book.fill_front(Side::Sell, 5);
  EXPECT_EQ(book.best_price(Side::Sell), 101);
  EXPECT_EQ(book.find(2), nullptr);
  book.fill_front(Side::Sell, 2);
  EXPECT_EQ(book.find(1)->qty, 3u);
  EXPECT_EQ(book.validate(), "");
}

TYPED_TEST(OrderBookTest, DetectsCrossedAndLockedBooks) {
  TypeParam book;
  ASSERT_TRUE(book.add(1, Side::Buy, 100, 1));
  ASSERT_TRUE(book.add(2, Side::Sell, 101, 1));
  EXPECT_FALSE(is_crossed(book));
  ASSERT_TRUE(book.add(3, Side::Buy, 101, 1));
  EXPECT_TRUE(is_crossed(book)) << "locked counts";
}

// Exercises free-list reuse in the pooled books: slots freed in one round are
// handed out again in the next, and queues must stay intact.
TYPED_TEST(OrderBookTest, ReusesFreedSlotsWithoutBreakingQueues) {
  TypeParam book;
  for (int round = 0; round < 3; ++round) {
    for (OrderId id = 1; id <= 50; ++id) {
      const Side side = id % 2 ? Side::Buy : Side::Sell;
      const Price offset = static_cast<Price>(id % 5);
      ASSERT_TRUE(book.add(id, side, side == Side::Buy ? 100 - offset : 200 + offset, 10));
    }
    for (OrderId id = 1; id <= 50; id += 3) ASSERT_TRUE(book.remove(id));
    ASSERT_EQ(book.validate(), "");
    for (OrderId id = 1; id <= 50; ++id) book.remove(id);
    ASSERT_EQ(book.order_count(), 0u);
    ASSERT_EQ(book.level_count(Side::Buy) + book.level_count(Side::Sell), 0u);
  }
}

}  // namespace
}  // namespace lob

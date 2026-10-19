#include <gtest/gtest.h>

#include "lob/map_order_book.hpp"

namespace lob {
namespace {

using Level = MapOrderBook::LevelInfo;

TEST(MapOrderBook, EmptyBook) {
  MapOrderBook book;
  EXPECT_EQ(book.order_count(), 0u);
  EXPECT_FALSE(book.best_price(Side::Buy));
  EXPECT_FALSE(book.best_price(Side::Sell));
  EXPECT_EQ(book.front(Side::Buy), nullptr);
  EXPECT_FALSE(is_crossed(book));
  EXPECT_EQ(book.validate(), "");
}

TEST(MapOrderBook, BestPricesAndLevelOrdering) {
  MapOrderBook book;
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
  EXPECT_EQ(book.validate(), "");
}

TEST(MapOrderBook, RejectsDuplicateIdAndZeroQuantity) {
  MapOrderBook book;
  ASSERT_TRUE(book.add(1, Side::Buy, 100, 10));
  EXPECT_FALSE(book.add(1, Side::Sell, 200, 10));
  EXPECT_FALSE(book.add(2, Side::Buy, 100, 0));
  EXPECT_EQ(book.order_count(), 1u);
}

TEST(MapOrderBook, ReducePartialThenToZeroRemovesOrderAndLevel) {
  MapOrderBook book;
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

TEST(MapOrderBook, RemoveFromMiddleKeepsFifoOrder) {
  MapOrderBook book;
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

TEST(MapOrderBook, FillFrontEmptiesLevelsInPriceOrder) {
  MapOrderBook book;
  ASSERT_TRUE(book.add(1, Side::Sell, 101, 5));
  ASSERT_TRUE(book.add(2, Side::Sell, 100, 5));
  book.fill_front(Side::Sell, 5);
  EXPECT_EQ(book.best_price(Side::Sell), 101);
  EXPECT_EQ(book.find(2), nullptr);
  book.fill_front(Side::Sell, 2);
  EXPECT_EQ(book.find(1)->qty, 3u);
  EXPECT_EQ(book.validate(), "");
}

TEST(MapOrderBook, DetectsCrossedAndLockedBooks) {
  MapOrderBook book;
  ASSERT_TRUE(book.add(1, Side::Buy, 100, 1));
  ASSERT_TRUE(book.add(2, Side::Sell, 101, 1));
  EXPECT_FALSE(is_crossed(book));
  ASSERT_TRUE(book.add(3, Side::Buy, 101, 1));
  EXPECT_TRUE(is_crossed(book)) << "locked counts";
}

}  // namespace
}  // namespace lob

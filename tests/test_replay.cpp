#include <gtest/gtest.h>

#include "lob/itch/encoder.hpp"
#include "lob/replay.hpp"

namespace lob {
namespace {

using itch::Encoder;
using Level = MapOrderBook::LevelInfo;

constexpr std::uint16_t kAapl = 13;
constexpr std::uint16_t kMsft = 42;

class ReplayTest : public ::testing::Test {
 protected:
  void SetUp() override {
    enc_.stock_directory(kAapl, 1, "AAPL");
    enc_.stock_directory(kMsft, 2, "MSFT");
  }
  void run() {
    const auto& b = enc_.bytes();
    ASSERT_EQ(itch::parse_buffer(b.data(), b.size(), replay_), b.size());
  }
  const MapOrderBook& book(std::uint16_t locate) {
    const MapOrderBook* b = replay_.books().find(locate);
    EXPECT_NE(b, nullptr);
    return *b;
  }

  Encoder enc_;
  Replayer<> replay_;
};

TEST_F(ReplayTest, MapsLocateCodesToSymbols) {
  run();
  EXPECT_EQ(replay_.locate_of("MSFT"), kMsft);
  EXPECT_EQ(replay_.symbol_of(kAapl), "AAPL");
  EXPECT_FALSE(replay_.locate_of("GOOG"));
  EXPECT_EQ(replay_.symbol_count(), 2u);
}

TEST_F(ReplayTest, BuildsSeparateBooksPerLocate) {
  enc_.add_order(kAapl, 10, 1, Side::Buy, 100, "AAPL", 1'000'000);
  enc_.add_order(kMsft, 11, 2, Side::Sell, 200, "MSFT", 4'000'000);
  enc_.add_order_mpid(kAapl, 12, 3, Side::Buy, 50, "AAPL", 1'000'000, "GSCO");
  run();
  EXPECT_EQ(book(kAapl).levels(Side::Buy), (std::vector<Level>{{1'000'000, 150, 2}}));
  EXPECT_EQ(book(kMsft).levels(Side::Sell), (std::vector<Level>{{4'000'000, 200, 1}}));
  EXPECT_EQ(replay_.stats().invalid_refs, 0u);
}

TEST_F(ReplayTest, ExecutionsCancelsAndDeletesReduceOrders) {
  enc_.add_order(kAapl, 10, 1, Side::Sell, 500, "AAPL", 1'000'100);
  enc_.add_order(kAapl, 11, 2, Side::Sell, 300, "AAPL", 1'000'100);
  enc_.order_executed(kAapl, 12, 1, 200, 1);
  enc_.order_executed_with_price(kAapl, 13, 1, 100, 2, 'Y', 1'000'000);
  enc_.order_cancel(kAapl, 14, 1, 150);
  enc_.order_executed(kAapl, 15, 1, 50, 3);  // fills the rest: order 1 leaves the book
  enc_.order_delete(kAapl, 16, 2);
  run();
  EXPECT_EQ(book(kAapl).order_count(), 0u);
  EXPECT_EQ(book(kAapl).level_count(Side::Sell), 0u);
  EXPECT_EQ(replay_.stats().executions, 3u);
  EXPECT_EQ(replay_.stats().executed_shares, 350u);
  EXPECT_EQ(replay_.stats().invalid_refs, 0u);
}

TEST_F(ReplayTest, ExecutionsConsumeOldestOrderFirstInTimePriority) {
  enc_.add_order(kAapl, 10, 1, Side::Buy, 100, "AAPL", 1'000'000);
  enc_.add_order(kAapl, 11, 2, Side::Buy, 100, "AAPL", 1'000'000);
  enc_.order_executed(kAapl, 12, 1, 100, 1);
  run();
  EXPECT_EQ(book(kAapl).front(Side::Buy)->id, 2u);
}

TEST_F(ReplayTest, ReplaceGetsNewIdKeepsSideAndLosesPriority) {
  enc_.add_order(kAapl, 10, 1, Side::Buy, 100, "AAPL", 1'000'000);
  enc_.add_order(kAapl, 11, 2, Side::Buy, 100, "AAPL", 1'000'000);
  enc_.order_replace(kAapl, 12, 1, 7, 300, 1'000'000);  // same price, now behind order 2
  run();
  const MapOrderBook& b = book(kAapl);
  EXPECT_EQ(b.find(1), nullptr);
  ASSERT_NE(b.find(7), nullptr);
  EXPECT_EQ(b.find(7)->side, Side::Buy);
  EXPECT_EQ(b.find(7)->qty, 300u);
  EXPECT_EQ(b.front(Side::Buy)->id, 2u);
  EXPECT_EQ(b.levels(Side::Buy), (std::vector<Level>{{1'000'000, 400, 2}}));
  EXPECT_EQ(b.validate(), "");
}

TEST_F(ReplayTest, ReplaceCanMovePrice) {
  enc_.add_order(kAapl, 10, 1, Side::Sell, 100, "AAPL", 1'000'500);
  enc_.order_replace(kAapl, 11, 1, 2, 100, 1'000'400);
  run();
  EXPECT_EQ(book(kAapl).best_price(Side::Sell), 1'000'400);
  EXPECT_EQ(book(kAapl).level_count(Side::Sell), 1u);
}

TEST_F(ReplayTest, CountsInvalidReferences) {
  enc_.add_order(kAapl, 10, 1, Side::Buy, 100, "AAPL", 1'000'000);
  enc_.order_executed(kAapl, 11, 99, 10, 1);   // unknown id
  enc_.order_cancel(kAapl, 12, 1, 101);        // more than remains
  enc_.order_delete(kMsft, 13, 1);             // right id, wrong stock
  enc_.order_replace(kAapl, 14, 98, 5, 1, 1);  // unknown original
  enc_.add_order(kAapl, 15, 1, Side::Buy, 1, "AAPL", 1);  // duplicate live id
  run();
  EXPECT_EQ(replay_.stats().invalid_refs, 5u);
  EXPECT_EQ(book(kAapl).find(1)->qty, 100u) << "invalid ops must not modify the book";
}

TEST_F(ReplayTest, TracksCrossedBooksOnlyDuringMarketHours) {
  enc_.add_order(kAapl, 10, 1, Side::Buy, 100, "AAPL", 1'000'100);
  enc_.add_order(kAapl, 11, 2, Side::Sell, 100, "AAPL", 1'000'000);  // crossed, pre-market
  enc_.system_event(0, 12, 'Q');
  enc_.add_order(kAapl, 13, 3, Side::Sell, 100, "AAPL", 1'000'000);  // still crossed, market open
  enc_.system_event(0, 14, 'M');
  enc_.add_order(kAapl, 15, 4, Side::Sell, 100, "AAPL", 1'000'000);
  run();
  EXPECT_EQ(replay_.stats().crossed_in_market_hours, 1u);
  EXPECT_EQ(replay_.stats().crossed_while_trading, 1u) << "no 'H' seen: treated as trading";
  ASSERT_EQ(replay_.crossings().size(), 1u);
  EXPECT_EQ(replay_.crossings()[0].timestamp, 13u);
  EXPECT_EQ(replay_.crossings()[0].bid, 1'000'100);
  EXPECT_EQ(replay_.crossings()[0].ask, 1'000'000);
  EXPECT_FALSE(replay_.market_open());
}

TEST_F(ReplayTest, RecordsTradingStateOfCrossings) {
  enc_.system_event(0, 9, 'Q');
  enc_.stock_trading_action(kAapl, 10, "AAPL", 'H');
  enc_.add_order(kAapl, 11, 1, Side::Buy, 100, "AAPL", 1'000'100);
  enc_.add_order(kAapl, 12, 2, Side::Sell, 100, "AAPL", 1'000'000);
  enc_.stock_trading_action(kAapl, 13, "AAPL", 'T');
  run();
  EXPECT_EQ(replay_.trading_state(kAapl), 'T');
  EXPECT_EQ(replay_.trading_state(kMsft), '?');
  ASSERT_EQ(replay_.crossings().size(), 1u);
  EXPECT_EQ(replay_.crossings()[0].trading_state, 'H');
  EXPECT_EQ(replay_.stats().crossed_while_trading, 0u);
}

TEST_F(ReplayTest, CountsMessagesByType) {
  enc_.add_order(kAapl, 10, 1, Side::Buy, 100, "AAPL", 1'000'000);
  enc_.raw('Y', kAapl, 11, 20);
  enc_.trade(kAapl, 12, 0, Side::Buy, 10, "AAPL", 1'000'000, 5);
  run();
  const ReplayStats& s = replay_.stats();
  EXPECT_EQ(s.messages, 5u);
  EXPECT_EQ(s.by_type['R'], 2u);
  EXPECT_EQ(s.by_type['A'], 1u);
  EXPECT_EQ(s.by_type['Y'], 1u);
  EXPECT_EQ(s.by_type['P'], 1u);
  EXPECT_EQ(s.book_ops, 1u);
  EXPECT_EQ(replay_.last_timestamp(), 12u);
}

}  // namespace
}  // namespace lob

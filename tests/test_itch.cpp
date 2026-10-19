#include <gtest/gtest.h>
#include <zlib.h>

#include <filesystem>
#include <string>
#include <vector>

#include "lob/itch/encoder.hpp"
#include "lob/itch/file.hpp"
#include "lob/itch/parser.hpp"

namespace lob::itch {
namespace {

// Records everything it sees, in order.
struct Recorder : NullHandler {
  std::vector<char> types;
  std::vector<Header> headers;
  std::vector<SystemEvent> system;
  std::vector<StockDirectory> directory;
  std::vector<AddOrder> adds;
  std::vector<OrderExecuted> executed;
  std::vector<OrderExecutedWithPrice> executed_px;
  std::vector<OrderCancel> cancels;
  std::vector<OrderDelete> deletes;
  std::vector<OrderReplace> replaces;
  std::vector<Trade> trades;
  std::vector<StockTradingAction> actions;
  int other = 0;
  int malformed = 0;

  void before_message(const Header& h) {
    types.push_back(h.type);
    headers.push_back(h);
  }
  void on_system_event(const SystemEvent& m) { system.push_back(m); }
  void on_stock_directory(const StockDirectory& m) { directory.push_back(m); }
  void on_add_order(const AddOrder& m) { adds.push_back(m); }
  void on_order_executed(const OrderExecuted& m) { executed.push_back(m); }
  void on_order_executed_with_price(const OrderExecutedWithPrice& m) { executed_px.push_back(m); }
  void on_order_cancel(const OrderCancel& m) { cancels.push_back(m); }
  void on_order_delete(const OrderDelete& m) { deletes.push_back(m); }
  void on_order_replace(const OrderReplace& m) { replaces.push_back(m); }
  void on_trade(const Trade& m) { trades.push_back(m); }
  void on_stock_trading_action(const StockTradingAction& m) { actions.push_back(m); }
  void on_other(const Header&) { ++other; }
  void on_malformed(char, std::size_t) { ++malformed; }
};

constexpr std::uint64_t kTs = 34'200'000'000'123ULL;  // 09:30:00.000000123, needs all 6 bytes

TEST(ItchParser, ByteSwapHelpers) {
  const std::uint8_t b[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  EXPECT_EQ(detail::be16(b), 0x0102u);
  EXPECT_EQ(detail::be32(b), 0x01020304u);
  EXPECT_EQ(detail::be48(b), 0x010203040506ULL);
  EXPECT_EQ(detail::be64(b), 0x0102030405060708ULL);
}

TEST(ItchParser, DecodesEveryBookMessage) {
  Encoder enc;
  enc.system_event(0, kTs, 'Q');
  enc.stock_directory(7, kTs, "AAPL");
  enc.add_order(7, kTs, 0x1122334455667788ULL, Side::Buy, 100, "AAPL", 1'234'500);
  enc.add_order_mpid(7, kTs + 1, 2, Side::Sell, 300, "AAPL", 1'234'600, "GSCO");
  enc.order_executed(7, kTs + 2, 2, 50, 999);
  enc.order_executed_with_price(7, kTs + 3, 2, 25, 1000, 'Y', 1'234'550);
  enc.order_cancel(7, kTs + 4, 2, 10);
  enc.order_delete(7, kTs + 5, 2);
  enc.order_replace(7, kTs + 6, 0x1122334455667788ULL, 3, 80, 1'234'400);
  enc.trade(7, kTs + 7, 0, Side::Sell, 5, "AAPL", 1'234'500, 1001);
  enc.stock_trading_action(7, kTs + 8, "AAPL", 'H', "LUDP");

  Recorder r;
  const auto& bytes = enc.bytes();
  ASSERT_EQ(parse_buffer(bytes.data(), bytes.size(), r), bytes.size());
  EXPECT_EQ(std::string(r.types.begin(), r.types.end()), "SRAFECXDUPH");
  EXPECT_EQ(r.malformed, 0);

  EXPECT_EQ(r.headers[0].timestamp, kTs);
  EXPECT_EQ(r.system.at(0).event_code, 'Q');
  EXPECT_EQ(r.directory.at(0).h.locate, 7);
  EXPECT_EQ(to_string(r.directory.at(0).stock), "AAPL");

  ASSERT_EQ(r.adds.size(), 2u);
  EXPECT_EQ(r.adds[0].ref, 0x1122334455667788ULL);
  EXPECT_EQ(r.adds[0].side, Side::Buy);
  EXPECT_EQ(r.adds[0].shares, 100u);
  EXPECT_EQ(r.adds[0].price, 1'234'500u);
  EXPECT_EQ(r.adds[1].h.type, 'F');
  EXPECT_EQ(r.adds[1].side, Side::Sell);
  EXPECT_EQ(r.adds[1].h.timestamp, kTs + 1);

  EXPECT_EQ(r.executed.at(0).ref, 2u);
  EXPECT_EQ(r.executed.at(0).shares, 50u);
  EXPECT_EQ(r.executed.at(0).match, 999u);
  EXPECT_EQ(r.executed_px.at(0).printable, 'Y');
  EXPECT_EQ(r.executed_px.at(0).price, 1'234'550u);
  EXPECT_EQ(r.cancels.at(0).shares, 10u);
  EXPECT_EQ(r.deletes.at(0).ref, 2u);
  EXPECT_EQ(r.replaces.at(0).old_ref, 0x1122334455667788ULL);
  EXPECT_EQ(r.replaces.at(0).new_ref, 3u);
  EXPECT_EQ(r.replaces.at(0).shares, 80u);
  EXPECT_EQ(r.replaces.at(0).price, 1'234'400u);
  EXPECT_EQ(r.trades.at(0).match, 1001u);
  EXPECT_EQ(to_string(r.actions.at(0).stock), "AAPL");
  EXPECT_EQ(r.actions.at(0).trading_state, 'H');
  EXPECT_EQ(std::string(r.actions.at(0).reason.data(), 4), "LUDP");
}

TEST(ItchParser, SkipsUndecodedTypesAndFlagsBadLengths) {
  Encoder enc;
  enc.raw('Y', 1, kTs, 20);   // Reg SHO restriction: valid, not decoded
  enc.raw('I', 1, kTs, 50);   // NOII
  enc.raw('A', 1, kTs, 30);   // add order with the wrong length
  enc.raw('Z', 1, kTs, 5);    // shorter than a header
  enc.order_delete(1, kTs, 9);

  Recorder r;
  const auto& bytes = enc.bytes();
  ASSERT_EQ(parse_buffer(bytes.data(), bytes.size(), r), bytes.size());
  EXPECT_EQ(r.other, 2);
  EXPECT_EQ(r.malformed, 2);
  EXPECT_EQ(r.deletes.size(), 1u);
}

TEST(ItchParser, StopsAtIncompleteMessage) {
  Encoder enc;
  enc.order_delete(1, kTs, 1);
  enc.order_delete(1, kTs, 2);
  const auto& bytes = enc.bytes();
  const std::size_t frame = 2 + 19;

  Recorder r;
  EXPECT_EQ(parse_buffer(bytes.data(), bytes.size() - 1, r), frame);
  EXPECT_EQ(r.deletes.size(), 1u);
  EXPECT_EQ(parse_buffer(bytes.data(), 1, r), 0u) << "half a length prefix";
}

TEST(ItchParser, HandlerCanStopEarly) {
  struct StopAfterTwo : NullHandler {
    int seen = 0;
    void before_message(const Header&) { ++seen; }
    bool stopped() const { return seen >= 2; }
  };
  Encoder enc;
  for (int i = 0; i < 5; ++i) enc.order_delete(1, kTs, static_cast<OrderId>(i));
  StopAfterTwo h;
  parse_buffer(enc.bytes().data(), enc.bytes().size(), h);
  EXPECT_EQ(h.seen, 2);
}

class GzFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = (std::filesystem::temp_directory_path() /
             ("lob_itch_test_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + ".gz"))
                .string();
    for (std::uint64_t i = 0; i < 1000; ++i) {
      enc_.add_order(3, kTs + i, i + 1, i % 2 ? Side::Buy : Side::Sell, 100, "MSFT", 4'000'000);
      enc_.order_delete(3, kTs + i, i + 1);
    }
    gzFile f = gzopen(path_.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    const auto& b = enc_.bytes();
    ASSERT_EQ(gzwrite(f, b.data(), static_cast<unsigned>(b.size())), static_cast<int>(b.size()));
    gzclose(f);
  }
  void TearDown() override { std::filesystem::remove(path_); }

  std::string path_;
  Encoder enc_;
};

TEST_F(GzFileTest, StreamsAcrossChunkBoundaries) {
  GzReader reader(path_);
  Recorder r;
  // 64-byte chunks force almost every message to straddle a boundary.
  EXPECT_EQ(stream_file(reader, r, 64), enc_.bytes().size());
  ASSERT_EQ(r.adds.size(), 1000u);
  ASSERT_EQ(r.deletes.size(), 1000u);
  EXPECT_EQ(r.adds[999].ref, 1000u);
  EXPECT_EQ(r.malformed, 0);
}

TEST_F(GzFileTest, LoadsAPrefixIntoMemory) {
  GzReader reader(path_);
  std::uint64_t loaded = 0;
  const auto bytes = load_messages(reader, 3, loaded);
  EXPECT_EQ(loaded, 3u);
  Recorder r;
  EXPECT_EQ(parse_buffer(bytes.data(), bytes.size(), r), bytes.size());
  EXPECT_EQ(r.types.size(), 3u);
}

TEST(GzReader, MissingFileThrows) {
  EXPECT_THROW(GzReader("/nonexistent/lob/file.gz"), std::runtime_error);
}

}  // namespace
}  // namespace lob::itch

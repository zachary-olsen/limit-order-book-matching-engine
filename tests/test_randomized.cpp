#include <gtest/gtest.h>

#include <cstdlib>
#include <random>
#include <string>
#include <unordered_map>

#include "book_types.hpp"
#include "lob/flow_generator.hpp"
#include "lob/matching_engine.hpp"
#include "lob/order_index.hpp"
#include "lob/replay.hpp"
#include "lob/synthetic_book_ops.hpp"
#include "reference_engine.hpp"

namespace lob {
namespace {

// LOB_RANDOM_OPS overrides the operation count, e.g. for a long soak run.
std::uint64_t op_count(std::uint64_t fallback) {
  if (const char* env = std::getenv("LOB_RANDOM_OPS")) return std::stoull(env);
  return fallback;
}

template <class Book>
class Randomized : public ::testing::Test {};
TYPED_TEST_SUITE(Randomized, testing::AllBooks, testing::BookNames);

TYPED_TEST(Randomized, InvariantsHoldAfterEveryOperation) {
  const std::uint64_t ops = op_count(1'000'000);
  FlowGenerator gen(42);
  MatchingEngine<TypeParam> engine;
  EventBuffer out;
  std::uint64_t trades = 0;

  for (std::uint64_t i = 0; i < ops; ++i) {
    const FlowOp op = gen.next();
    out.clear();
    apply_op(engine, op, out);

    const TypeParam& book = engine.book();
    const std::string err = book.validate();
    ASSERT_EQ(err, "") << "after op " << i;
    ASSERT_FALSE(is_crossed(book)) << "crossed after op " << i;

    // Events must describe a consistent outcome: exactly one Accepted or
    // Rejected first, and trades at prices the incoming order allows.
    ASSERT_FALSE(out.empty());
    if (op.kind != FlowOp::Kind::Cancel) {
      ASSERT_TRUE(out[0].type == EventType::Accepted || out[0].type == EventType::Rejected) << "op " << i;
    }
    for (const Event& e : out) {
      if (e.type != EventType::Trade) continue;
      ++trades;
      ASSERT_GT(e.qty, 0u);
      if (op.kind == FlowOp::Kind::Limit) {
        ASSERT_TRUE(crosses(op.side, op.price, e.price)) << "op " << i;
      }
    }
  }
  EXPECT_GT(trades, ops / 10) << "flow should exercise matching heavily";
}

// Replay-style differential: the same ITCH book ops, including invalid ones,
// must give the same results and the same book in every implementation.
TYPED_TEST(Randomized, MatchesBaselineOnItchStyleOps) {
  auto ops = synthetic_book_ops(op_count(300'000), 5);
  // Corrupt about 1% of ops so rejections are compared too.
  std::mt19937_64 rng(9);
  for (BookOp& op : ops) {
    if (rng() % 100 != 0) continue;
    switch (rng() % 3) {
      case 0: op.ref += 1'000'000'000; break;  // unknown id
      case 1: op.shares += 1'000'000; break;   // more shares than rest
      default:                                 // re-add: duplicate id if still live
        op.kind = BookOpKind::Add;
        op.side = Side::Buy;
        op.price = 990'000;
        op.shares = 1;
        break;
    }
  }

  BookSet<MapOrderBook> baseline;
  BookSet<TypeParam> books;
  for (std::size_t i = 0; i < ops.size(); ++i) {
    ASSERT_EQ(books.apply(ops[i]), baseline.apply(ops[i])) << "op " << i;
    const TypeParam& got = books.book(1);
    const MapOrderBook& want = baseline.book(1);
    ASSERT_EQ(got.order_count(), want.order_count()) << "op " << i;
    ASSERT_EQ(got.best_price(Side::Buy), want.best_price(Side::Buy)) << "op " << i;
    ASSERT_EQ(got.best_price(Side::Sell), want.best_price(Side::Sell)) << "op " << i;
    if (i % 1000 == 0) {
      ASSERT_EQ(got.validate(), "") << "op " << i;
      ASSERT_EQ(got.levels(Side::Buy), want.levels(Side::Buy)) << "op " << i;
      ASSERT_EQ(got.levels(Side::Sell), want.levels(Side::Sell)) << "op " << i;
    }
  }
}

struct DiffCase {
  const char* name;
  std::uint64_t seed;
  std::uint64_t ops;
  FlowParams params;
};

void PrintTo(const DiffCase& c, std::ostream* os) { *os << c.name << " (seed " << c.seed << ")"; }

// The real engine and the dumb reference must produce identical event streams
// and agree on top of book after every operation.
template <class Book>
void expect_matches_reference(const DiffCase& c) {
  SCOPED_TRACE(testing::book_name<Book>());
  FlowGenerator gen(c.seed, c.params);
  MatchingEngine<Book> engine;
  testing::ReferenceEngine reference;
  EventBuffer got;
  EventBuffer want;

  const std::uint64_t ops = op_count(c.ops);
  for (std::uint64_t i = 0; i < ops; ++i) {
    const FlowOp op = gen.next();
    got.clear();
    want.clear();
    apply_op(engine, op, got);
    apply_op(reference, op, want);

    ASSERT_EQ(got.size(), want.size()) << "op " << i;
    for (std::size_t k = 0; k < got.size(); ++k) ASSERT_EQ(got[k], want[k]) << "op " << i << " event " << k;
    ASSERT_EQ(engine.book().order_count(), reference.order_count()) << "op " << i;
    ASSERT_EQ(engine.book().best_price(Side::Buy), reference.best_price(Side::Buy)) << "op " << i;
    ASSERT_EQ(engine.book().best_price(Side::Sell), reference.best_price(Side::Sell)) << "op " << i;
  }
}

class Differential : public ::testing::TestWithParam<DiffCase> {};

TEST_P(Differential, MatchesReferenceEngine) {
  expect_matches_reference<MapOrderBook>(GetParam());
  expect_matches_reference<PoolOrderBook>(GetParam());
  expect_matches_reference<PoolFlatOrderBook>(GetParam());
  expect_matches_reference<VectorOrderBook>(GetParam());
}

FlowParams market_heavy() {
  FlowParams p;
  p.limit_share = 0.5;
  p.market_share = 0.3;
  return p;
}
FlowParams deep_book() {
  FlowParams p;
  p.limit_share = 0.7;
  p.market_share = 0.02;
  p.half_band = 200;
  p.max_open = 2'000;
  return p;
}
FlowParams tight_and_noisy() {
  FlowParams p;
  p.half_band = 2;
  p.max_qty = 10;
  p.invalid_share = 0.05;
  return p;
}

INSTANTIATE_TEST_SUITE_P(Flows, Differential,
                         ::testing::Values(DiffCase{"Default", 7, 1'000'000, FlowParams{}},
                                           DiffCase{"MarketHeavy", 11, 200'000, market_heavy()},
                                           DiffCase{"DeepBook", 13, 50'000, deep_book()},
                                           DiffCase{"TightAndNoisy", 17, 200'000, tight_and_noisy()}),
                         [](const ::testing::TestParamInfo<DiffCase>& p) { return p.param.name; });

// FlatIndex against std::unordered_map, with a key space small enough that
// probe chains collide, wrap around the table and get backward-shifted.
TEST(FlatIndex, MatchesStdUnorderedMap) {
  FlatIndex index(8);
  std::unordered_map<OrderId, std::uint32_t> want;
  std::mt19937_64 rng(1);
  for (int i = 0; i < 500'000; ++i) {
    const OrderId key = rng() % 2'000;
    switch (rng() % 3) {
      case 0: {
        auto [slot, inserted] = index.try_emplace(key);
        ASSERT_EQ(inserted, want.find(key) == want.end()) << "op " << i;
        if (inserted) *slot = want[key] = static_cast<std::uint32_t>(i);
        break;
      }
      case 1: ASSERT_EQ(index.erase(key), want.erase(key) == 1) << "op " << i; break;
      default: {
        const std::uint32_t* got = index.find(key);
        auto it = want.find(key);
        ASSERT_EQ(got != nullptr, it != want.end()) << "op " << i;
        if (got) {
          ASSERT_EQ(*got, it->second) << "op " << i;
        }
      }
    }
    ASSERT_EQ(index.size(), want.size());
  }
  for (const auto& [key, value] : want) {
    ASSERT_NE(index.find(key), nullptr);
    EXPECT_EQ(*index.find(key), value);
  }
}

}  // namespace
}  // namespace lob

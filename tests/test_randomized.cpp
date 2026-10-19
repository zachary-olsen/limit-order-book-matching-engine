#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "lob/flow_generator.hpp"
#include "lob/matching_engine.hpp"
#include "reference_engine.hpp"

namespace lob {
namespace {

// LOB_RANDOM_OPS overrides the operation count, e.g. for a long soak run.
std::uint64_t op_count(std::uint64_t fallback) {
  if (const char* env = std::getenv("LOB_RANDOM_OPS")) return std::stoull(env);
  return fallback;
}

TEST(Randomized, InvariantsHoldAfterEveryOperation) {
  const std::uint64_t ops = op_count(1'000'000);
  FlowGenerator gen(42);
  MatchingEngine<> engine;
  EventBuffer out;
  std::uint64_t trades = 0;

  for (std::uint64_t i = 0; i < ops; ++i) {
    const FlowOp op = gen.next();
    out.clear();
    apply_op(engine, op, out);

    const MapOrderBook& book = engine.book();
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

struct DiffCase {
  const char* name;
  std::uint64_t seed;
  std::uint64_t ops;
  FlowParams params;
};

void PrintTo(const DiffCase& c, std::ostream* os) { *os << c.name << " (seed " << c.seed << ")"; }

class Differential : public ::testing::TestWithParam<DiffCase> {};

// The real engine and the dumb reference must produce identical event streams
// and agree on top of book after every operation.
TEST_P(Differential, MatchesReferenceEngine) {
  const DiffCase& c = GetParam();
  FlowGenerator gen(c.seed, c.params);
  MatchingEngine<> engine;
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

}  // namespace
}  // namespace lob

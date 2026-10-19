#include <gtest/gtest.h>

#include <memory>

#include "lob/latency_histogram.hpp"

namespace lob {
namespace {

using H = LatencyHistogram;

TEST(LatencyHistogram, SmallValuesAreExact) {
  for (std::uint64_t v = 0; v < H::kSub * 2; ++v) {
    EXPECT_EQ(H::lower_bound(H::bucket_of(v)), v);
    EXPECT_EQ(H::upper_bound(H::bucket_of(v)), v);
  }
}

TEST(LatencyHistogram, BucketsContainTheirValuesWithBoundedError) {
  for (std::uint64_t v : {64ULL, 65ULL, 100ULL, 1'000ULL, 12'345ULL, 1ULL << 40, ~0ULL}) {
    const std::size_t b = H::bucket_of(v);
    ASSERT_LT(b, H::kBuckets);
    EXPECT_LE(H::lower_bound(b), v);
    EXPECT_GE(H::upper_bound(b), v);
    const double width = static_cast<double>(H::upper_bound(b) - H::lower_bound(b) + 1);
    EXPECT_LE(width / static_cast<double>(v), 1.0 / H::kSub + 1e-12);
  }
}

TEST(LatencyHistogram, Percentiles) {
  auto h = std::make_unique<H>();
  for (std::uint64_t v = 1; v <= 1000; ++v) h->record(v);
  EXPECT_EQ(h->count(), 1000u);
  EXPECT_EQ(h->min(), 1u);
  EXPECT_EQ(h->max(), 1000u);
  EXPECT_DOUBLE_EQ(h->mean(), 500.5);
  EXPECT_NEAR(static_cast<double>(h->percentile(0.5)), 500, 500 / 32.0);
  EXPECT_NEAR(static_cast<double>(h->percentile(0.99)), 990, 990 / 32.0);
  EXPECT_EQ(h->percentile(1.0), 1000u);
}

TEST(LatencyHistogram, TailIsVisible) {
  auto h = std::make_unique<H>();
  for (int i = 0; i < 999; ++i) h->record(50);
  h->record(1'000'000);
  EXPECT_EQ(h->percentile(0.5), 50u);
  EXPECT_EQ(h->percentile(0.999), 50u);
  EXPECT_EQ(h->percentile(0.9999), 1'000'000u);
}

TEST(LatencyHistogram, Merge) {
  auto a = std::make_unique<H>();
  auto b = std::make_unique<H>();
  a->record(10);
  b->record(20);
  a->merge(*b);
  EXPECT_EQ(a->count(), 2u);
  EXPECT_EQ(a->min(), 10u);
  EXPECT_EQ(a->max(), 20u);
}

}  // namespace
}  // namespace lob

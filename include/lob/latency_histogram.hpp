#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace lob {

// Log-linear histogram (the HdrHistogram idea): values below 2^kSubBits are
// exact; above that each power of two is split into 2^kSubBits buckets, so the
// relative error stays under 1 / 2^kSubBits (about 3%). Recording is a few
// instructions and never allocates.
class LatencyHistogram {
 public:
  static constexpr unsigned kSubBits = 5;
  static constexpr std::size_t kSub = std::size_t{1} << kSubBits;
  static constexpr std::size_t kBuckets = (64 - kSubBits + 1) * kSub;

  void record(std::uint64_t v) noexcept {
    ++counts_[bucket_of(v)];
    ++total_;
    sum_ += v;
    min_ = std::min(min_, v);
    max_ = std::max(max_, v);
  }

  void merge(const LatencyHistogram& other) noexcept {
    for (std::size_t i = 0; i < kBuckets; ++i) counts_[i] += other.counts_[i];
    total_ += other.total_;
    sum_ += other.sum_;
    min_ = std::min(min_, other.min_);
    max_ = std::max(max_, other.max_);
  }

  // Smallest bucket upper bound such that at least q (0..1] of samples are
  // at or below it, clamped to the observed maximum.
  [[nodiscard]] std::uint64_t percentile(double q) const noexcept {
    if (total_ == 0) return 0;
    const double target = q * static_cast<double>(total_);
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < kBuckets; ++i) {
      seen += counts_[i];
      if (seen > 0 && static_cast<double>(seen) >= target) return std::min(upper_bound(i), max_);
    }
    return max_;
  }

  [[nodiscard]] std::uint64_t count() const noexcept { return total_; }
  [[nodiscard]] std::uint64_t min() const noexcept { return total_ ? min_ : 0; }
  [[nodiscard]] std::uint64_t max() const noexcept { return max_; }
  [[nodiscard]] double mean() const noexcept {
    return total_ ? static_cast<double>(sum_) / static_cast<double>(total_) : 0.0;
  }

  [[nodiscard]] std::uint64_t bucket_count(std::size_t i) const noexcept { return counts_[i]; }

  static constexpr std::size_t bucket_of(std::uint64_t v) noexcept {
    if (v < kSub) return static_cast<std::size_t>(v);
    const unsigned msb = 63U - static_cast<unsigned>(std::countl_zero(v));
    const unsigned shift = msb - kSubBits;
    const std::size_t sub = static_cast<std::size_t>(v >> shift) & (kSub - 1);
    return (shift + 1) * kSub + sub;
  }

  static constexpr std::uint64_t lower_bound(std::size_t i) noexcept {
    if (i < kSub) return i;
    const std::size_t shift = i / kSub - 1;
    return (kSub + i % kSub) << shift;
  }

  static constexpr std::uint64_t upper_bound(std::size_t i) noexcept {
    if (i < kSub) return i;
    const std::size_t shift = i / kSub - 1;
    return lower_bound(i) + ((std::uint64_t{1} << shift) - 1);
  }

 private:
  std::array<std::uint64_t, kBuckets> counts_{};
  std::uint64_t total_ = 0;
  std::uint64_t sum_ = 0;
  std::uint64_t min_ = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t max_ = 0;
};

}  // namespace lob

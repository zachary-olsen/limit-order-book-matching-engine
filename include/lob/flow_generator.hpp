#pragma once

#include <cstdint>
#include <random>
#include <vector>

#include "lob/events.hpp"
#include "lob/types.hpp"

namespace lob {

struct FlowOp {
  enum class Kind : std::uint8_t { Limit, Market, Cancel };
  Kind kind;
  Side side;
  OrderId id;
  Price price;
  Qty qty;
};

struct FlowParams {
  double limit_share = 0.55;   // fraction of ops that are limit orders
  double market_share = 0.10;  // fraction that are market orders; the rest cancel
  Price start_mid = 10'000;
  Price half_band = 25;        // limit prices are uniform in mid +/- half_band
  Qty max_qty = 1'000;
  std::size_t max_open = 256;  // limit ids not yet cancelled; at the cap, the next op is a cancel
  double invalid_share = 0.005;  // zero qty, bad price or duplicate id
};

// Deeper book (thousands of resting orders over ~200 levels) for benchmarks.
inline FlowParams benchmark_flow() {
  FlowParams p;
  p.half_band = 100;
  p.max_open = 10'000;
  p.invalid_share = 0;
  return p;
}

// Deterministic random order flow. Uses its own integer-to-range mapping
// rather than std::uniform_*_distribution so a seed gives the same stream on
// every standard library.
//
// Cancels target a random limit id the generator issued and has not yet
// cancelled. Some of those were already filled, which exercises unknown-id
// rejects. Every resting order is in that open set, so max_open bounds the
// size of the book.
class FlowGenerator {
 public:
  explicit FlowGenerator(std::uint64_t seed, FlowParams params = {})
      : rng_(seed), params_(params), mid_(params.start_mid) {}

  FlowOp next() {
    if ((++count_ & 1023) == 0) drift_mid();
    FlowOp op{};
    op.side = coin() ? Side::Buy : Side::Sell;
    const double r = unit();
    if (open_.size() >= params_.max_open || (r >= params_.limit_share + params_.market_share && !open_.empty())) {
      op.kind = FlowOp::Kind::Cancel;
      const std::size_t i = random_open();
      op.id = open_[i];
      open_[i] = open_.back();
      open_.pop_back();
    } else if (r < params_.limit_share) {
      op.kind = FlowOp::Kind::Limit;
      op.id = next_id_++;
      op.price = mid_ + range(-params_.half_band, params_.half_band);
      op.qty = static_cast<Qty>(range(1, params_.max_qty));
      open_.push_back(op.id);
    } else if (r < params_.limit_share + params_.market_share) {
      op.kind = FlowOp::Kind::Market;
      op.id = next_id_++;
      op.qty = static_cast<Qty>(range(1, params_.max_qty));
    } else {
      op.kind = FlowOp::Kind::Cancel;
      op.id = next_id_ + 1'000'000;  // nothing open: an id that was never issued
    }
    if (unit() < params_.invalid_share) corrupt(op);
    return op;
  }

 private:
  std::int64_t range(std::int64_t lo, std::int64_t hi) {
    const auto span = static_cast<std::uint64_t>(hi - lo) + 1;
    return lo + static_cast<std::int64_t>(rng_() % span);
  }
  double unit() { return static_cast<double>(rng_() >> 11) * 0x1.0p-53; }
  bool coin() { return (rng_() >> 63) != 0; }
  std::size_t random_open() { return static_cast<std::size_t>(rng_() % open_.size()); }

  void drift_mid() {
    mid_ += coin() ? 1 : -1;
    if (mid_ <= params_.half_band) mid_ = params_.half_band + 1;
  }

  void corrupt(FlowOp& op) {
    switch (range(0, 2)) {
      case 0: op.qty = 0; break;
      case 1: op.price = -op.price; break;  // only matters for limits
      default:  // reuse an open id: a duplicate if that order still rests
        if (!open_.empty()) op.id = open_[random_open()];
        break;
    }
  }

  std::mt19937_64 rng_;
  FlowParams params_;
  Price mid_;
  std::vector<OrderId> open_;
  OrderId next_id_ = 1;
  std::uint64_t count_ = 0;
};

// Applies an op to anything with the MatchingEngine interface.
template <class Engine>
void apply_op(Engine& engine, const FlowOp& op, EventBuffer& out) {
  switch (op.kind) {
    case FlowOp::Kind::Limit: engine.submit_limit(op.id, op.side, op.price, op.qty, out); break;
    case FlowOp::Kind::Market: engine.submit_market(op.id, op.side, op.qty, out); break;
    case FlowOp::Kind::Cancel: engine.cancel(op.id, out); break;
  }
}

}  // namespace lob

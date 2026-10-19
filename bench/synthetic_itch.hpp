#pragma once

#include <cstdint>
#include <random>
#include <vector>

#include "lob/itch/encoder.hpp"
#include "lob/replay.hpp"

namespace lob::bench {

// A self-consistent stream of book ops for one symbol (every reference is to
// a live order with enough shares), for benchmarking without an ITCH file.
// The mix loosely follows a real day: mostly adds and deletes, fewer executions.
inline std::vector<BookOp> synthetic_book_ops(std::size_t n, std::uint64_t seed = 1) {
  struct Live {
    OrderId id;
    Qty shares;
    Side side;
    Price price;
  };
  constexpr std::uint16_t kLocate = 1;
  constexpr Price kMid = 1'000'000;  // $100.0000
  constexpr Price kTick = 100;       // $0.01

  std::mt19937_64 rng(seed);
  std::vector<BookOp> ops;
  ops.reserve(n);
  std::vector<Live> live;
  OrderId next_id = 1;

  auto pick = [&] { return static_cast<std::size_t>(rng() % live.size()); };
  auto drop = [&](std::size_t i) {
    live[i] = live.back();
    live.pop_back();
  };
  auto random_shares = [&] { return static_cast<Qty>(100 * (1 + rng() % 10)); };
  // Bids at or below mid - 1 tick, asks at or above mid: never crossed.
  auto random_price = [&](Side side) {
    const Price offset = static_cast<Price>(rng() % 50) * kTick;
    return side == Side::Buy ? kMid - kTick - offset : kMid + offset;
  };

  while (ops.size() < n) {
    const unsigned r = static_cast<unsigned>(rng() % 100);
    if (live.size() < 64 || r < 45) {
      const Side side = (rng() & 1) ? Side::Buy : Side::Sell;
      const Live o{next_id++, random_shares(), side, random_price(side)};
      ops.push_back({o.id, 0, o.price, o.shares, kLocate, BookOpKind::Add, side});
      live.push_back(o);
    } else if (r < 85) {
      const std::size_t i = pick();
      ops.push_back({live[i].id, 0, 0, 0, kLocate, BookOpKind::Delete, Side::Buy});
      drop(i);
    } else if (r < 92) {
      const std::size_t i = pick();
      const Qty q = std::min<Qty>(live[i].shares, 100);
      ops.push_back({live[i].id, 0, 0, q, kLocate, BookOpKind::Execute, Side::Buy});
      if ((live[i].shares -= q) == 0) drop(i);
    } else if (r < 96) {
      const std::size_t i = pick();
      if (live[i].shares <= 100) continue;
      ops.push_back({live[i].id, 0, 0, 100, kLocate, BookOpKind::Cancel, Side::Buy});
      live[i].shares -= 100;
    } else {
      const std::size_t i = pick();
      const Live o{next_id++, random_shares(), live[i].side, random_price(live[i].side)};
      ops.push_back({live[i].id, o.id, o.price, o.shares, kLocate, BookOpKind::Replace, Side::Buy});
      live[i] = o;
    }
  }
  return ops;
}

// Encodes book ops as ITCH messages, for parser benchmarks.
inline std::vector<std::uint8_t> encode(const std::vector<BookOp>& ops) {
  itch::Encoder enc;
  std::uint64_t ts = 34'200'000'000'000ULL;
  for (const BookOp& op : ops) {
    ++ts;
    const auto price = static_cast<std::uint32_t>(op.price);
    switch (op.kind) {
      case BookOpKind::Add: enc.add_order(op.locate, ts, op.ref, op.side, op.shares, "SYNTH", price); break;
      case BookOpKind::Execute: enc.order_executed(op.locate, ts, op.ref, op.shares, ts); break;
      case BookOpKind::Cancel: enc.order_cancel(op.locate, ts, op.ref, op.shares); break;
      case BookOpKind::Delete: enc.order_delete(op.locate, ts, op.ref); break;
      case BookOpKind::Replace: enc.order_replace(op.locate, ts, op.ref, op.new_ref, op.shares, price); break;
    }
  }
  return enc.bytes();
}

}  // namespace lob::bench

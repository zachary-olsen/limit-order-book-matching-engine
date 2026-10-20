#include "lob/map_order_book.hpp"

#include <cassert>
#include <iterator>
#include <sstream>

namespace lob {

bool MapOrderBook::add(OrderId id, Side side, Price price, Qty qty) {
  if (qty == 0) return false;
  auto [slot, inserted] = index_.try_emplace(id);
  if (!inserted) return false;
  on_side(*this, side, [&](auto& levels) {
    Level& level = levels[price];
    level.orders.push_back(Order{id, price, qty, side, next_seq_++});
    level.total += qty;
    slot->second = Location{std::prev(level.orders.end()), &level};
  });
  return true;
}

bool MapOrderBook::reduce(OrderId id, Qty qty) {
  auto pos = index_.find(id);
  if (pos == index_.end()) return false;
  Order& order = *pos->second.it;
  if (qty > order.qty) return false;
  if (qty == order.qty) {
    erase(pos);
  } else {
    order.qty -= qty;
    pos->second.level->total -= qty;
  }
  return true;
}

bool MapOrderBook::remove(OrderId id) {
  auto pos = index_.find(id);
  if (pos == index_.end()) return false;
  erase(pos);
  return true;
}

void MapOrderBook::fill_front(Side side, Qty qty) {
  on_side(*this, side, [&](auto& levels) {
    assert(!levels.empty());
    auto best = levels.begin();
    Level& level = best->second;
    Order& order = level.orders.front();
    assert(qty <= order.qty);
    order.qty -= qty;
    level.total -= qty;
    if (order.qty == 0) {
      index_.erase(order.id);
      level.orders.pop_front();
      if (level.orders.empty()) levels.erase(best);
    }
  });
}

void MapOrderBook::erase(Index::iterator pos) {
  auto [it, level] = pos->second;
  const Side side = it->side;
  const Price price = it->price;
  level->total -= it->qty;
  level->orders.erase(it);
  index_.erase(pos);
  if (level->orders.empty()) {
    on_side(*this, side, [price](auto& levels) { levels.erase(price); });
  }
}

const Order* MapOrderBook::find(OrderId id) const {
  auto pos = index_.find(id);
  return pos == index_.end() ? nullptr : &*pos->second.it;
}

const Order* MapOrderBook::front(Side side) const {
  return on_side(*this, side, [](const auto& levels) -> const Order* {
    return levels.empty() ? nullptr : &levels.begin()->second.orders.front();
  });
}

std::optional<Price> MapOrderBook::best_price(Side side) const {
  return on_side(*this, side, [](const auto& levels) -> std::optional<Price> {
    if (levels.empty()) return std::nullopt;
    return levels.begin()->first;
  });
}

std::size_t MapOrderBook::level_count(Side side) const noexcept {
  return side == Side::Buy ? bids_.size() : asks_.size();
}

std::uint64_t MapOrderBook::depth_at(Side side, Price price) const {
  return on_side(*this, side, [price](const auto& levels) -> std::uint64_t {
    auto it = levels.find(price);
    return it == levels.end() ? 0 : it->second.total;
  });
}

std::vector<MapOrderBook::LevelInfo> MapOrderBook::levels(Side side, std::size_t max_levels) const {
  std::vector<LevelInfo> out;
  on_side(*this, side, [&](const auto& levels) {
    for (const auto& [price, level] : levels) {
      if (out.size() >= max_levels) break;
      out.push_back({price, level.total, level.orders.size()});
    }
  });
  return out;
}

std::string MapOrderBook::validate() const {
  std::ostringstream err;
  std::size_t resting = 0;

  auto check_side = [&](const auto& levels, Side side) {
    for (const auto& [price, level] : levels) {
      if (level.orders.empty()) {
        err << "empty level at " << price;
        return;
      }
      std::uint64_t sum = 0;
      for (auto it = level.orders.begin(); it != level.orders.end(); ++it) {
        const Order& o = *it;
        if (o.side != side || o.price != price) {
          err << "order " << o.id << " filed under the wrong side or price " << price;
          return;
        }
        if (o.qty == 0) {
          err << "order " << o.id << " rests with zero quantity";
          return;
        }
        if (it != level.orders.begin() && std::prev(it)->seq >= o.seq) {
          err << "time priority broken at price " << price << " order " << o.id;
          return;
        }
        auto idx = index_.find(o.id);
        if (idx == index_.end() || idx->second.it != it || idx->second.level != &level) {
          err << "index entry missing or stale for order " << o.id;
          return;
        }
        sum += o.qty;
        ++resting;
      }
      if (sum != level.total) {
        err << "level " << price << " total " << level.total << " != sum of orders " << sum;
        return;
      }
    }
  };

  check_side(bids_, Side::Buy);
  if (err.tellp() == 0) check_side(asks_, Side::Sell);
  if (err.tellp() == 0 && resting != index_.size()) {
    err << "index holds " << index_.size() << " ids but " << resting << " orders rest";
  }
  return err.str();
}

}  // namespace lob

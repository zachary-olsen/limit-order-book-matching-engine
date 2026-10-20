#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <type_traits>
#include <utility>
#include <vector>

#include "lob/types.hpp"

// Price -> level-slot mapping for one side of a book, best price first. Two
// implementations with the same interface (Bids selects the ordering):
//
//   try_emplace(price)  {pointer to the slot, true} if inserted, {existing, false} if present
//   find(price)         pointer to the slot, or nullptr
//   erase(price)        remove a price that is present
//   best()              slot of the best price (precondition: not empty)
//   for_each(f)         f(price, slot) best-first until f returns false
namespace lob {

// Red-black tree: O(log L) everywhere, one heap node per level.
template <bool Bids>
class MapLevels {
 public:
  std::pair<std::uint32_t*, bool> try_emplace(Price price) {
    auto [it, inserted] = map_.try_emplace(price, 0);
    return {&it->second, inserted};
  }
  [[nodiscard]] const std::uint32_t* find(Price price) const {
    auto it = map_.find(price);
    return it == map_.end() ? nullptr : &it->second;
  }
  void erase(Price price) { map_.erase(price); }
  [[nodiscard]] std::uint32_t best() const { return map_.begin()->second; }
  [[nodiscard]] bool empty() const noexcept { return map_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }

  template <class F>
  void for_each(F&& f) const {
    for (const auto& [price, slot] : map_) {
      if (!f(price, slot)) return;
    }
  }

 private:
  using Better = std::conditional_t<Bids, std::greater<Price>, std::less<Price>>;
  std::map<Price, std::uint32_t, Better> map_;
};

// Sorted contiguous array with the best price at the back. Most activity is
// near the top of the book, so inserting or erasing a level shifts only the
// few entries behind it, and the best level is always v.back().
template <bool Bids>
class VectorLevels {
 public:
  std::pair<std::uint32_t*, bool> try_emplace(Price price) {
    auto it = position(price);
    if (it != levels_.end() && it->first == price) return {&it->second, false};
    it = levels_.insert(it, {price, 0});
    return {&it->second, true};
  }
  [[nodiscard]] const std::uint32_t* find(Price price) const {
    auto it = position(levels_, price);
    return it != levels_.end() && it->first == price ? &it->second : nullptr;
  }
  void erase(Price price) {
    if (levels_.back().first == price) {
      levels_.pop_back();
      return;
    }
    levels_.erase(position(price));
  }
  [[nodiscard]] std::uint32_t best() const { return levels_.back().second; }
  [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return levels_.size(); }

  template <class F>
  void for_each(F&& f) const {
    for (auto it = levels_.rbegin(); it != levels_.rend(); ++it) {
      if (!f(it->first, it->second)) return;
    }
  }

 private:
  // Ascending by quality: bids ascending in price, asks descending.
  static bool worse(Price a, Price b) noexcept { return Bids ? a < b : a > b; }

  // First entry not worse than price.
  template <class Vec>
  static auto position(Vec& levels, Price price) {
    return std::lower_bound(levels.begin(), levels.end(), price,
                            [](const std::pair<Price, std::uint32_t>& e, Price p) { return worse(e.first, p); });
  }
  auto position(Price price) { return position(levels_, price); }

  std::vector<std::pair<Price, std::uint32_t>> levels_;
};

}  // namespace lob

#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "lob/types.hpp"

// Maps order ids to slots in a book's order pool. Two implementations with
// the same interface, so their cost can be compared in isolation:
//
//   try_emplace(id)  {pointer to the value, true} if inserted, {existing, false} if present
//   find(id)         pointer to the value, or nullptr
//   erase(id)        true if the id was present
namespace lob {

// std::unordered_map: one heap node per entry, a pointer chase per lookup.
class StdIndex {
 public:
  std::pair<std::uint32_t*, bool> try_emplace(OrderId id) {
    auto [it, inserted] = map_.try_emplace(id, 0);
    return {&it->second, inserted};
  }
  std::uint32_t* find(OrderId id) {
    auto it = map_.find(id);
    return it == map_.end() ? nullptr : &it->second;
  }
  const std::uint32_t* find(OrderId id) const {
    auto it = map_.find(id);
    return it == map_.end() ? nullptr : &it->second;
  }
  bool erase(OrderId id) { return map_.erase(id) != 0; }
  [[nodiscard]] std::size_t size() const noexcept { return map_.size(); }

 private:
  std::unordered_map<OrderId, std::uint32_t> map_;
};

// Open addressing with linear probing in one flat array: a lookup is usually
// a single cache line. Fibonacci hashing spreads sequential ids (ITCH order
// references are close to sequential). Deletion shifts later entries back
// instead of leaving tombstones, so probe chains never degrade. Load factor
// stays at or below 1/2.
class FlatIndex {
 public:
  explicit FlatIndex(std::size_t capacity = 64) { rehash(round_up_pow2(capacity < 8 ? 8 : capacity)); }

  std::pair<std::uint32_t*, bool> try_emplace(OrderId id) {
    if ((size_ + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);
    for (std::size_t i = home(id);; i = (i + 1) & mask_) {
      Slot& s = slots_[i];
      if (s.value == kEmpty) {
        s.key = id;
        s.value = 0;
        ++size_;
        return {&s.value, true};
      }
      if (s.key == id) return {&s.value, false};
    }
  }

  std::uint32_t* find(OrderId id) {
    const std::size_t i = locate(id);
    return i == kNotFound ? nullptr : &slots_[i].value;
  }
  const std::uint32_t* find(OrderId id) const {
    const std::size_t i = locate(id);
    return i == kNotFound ? nullptr : &slots_[i].value;
  }

  bool erase(OrderId id) {
    std::size_t hole = locate(id);
    if (hole == kNotFound) return false;
    // Backward-shift deletion: pull later entries of the probe chain into the
    // hole whenever their home slot is not cyclically within (hole, j].
    for (std::size_t j = (hole + 1) & mask_; slots_[j].value != kEmpty; j = (j + 1) & mask_) {
      const std::size_t k = home(slots_[j].key);
      const bool k_in_range = hole < j ? (hole < k && k <= j) : (hole < k || k <= j);
      if (!k_in_range) {
        slots_[hole] = slots_[j];
        hole = j;
      }
    }
    slots_[hole].value = kEmpty;
    --size_;
    return true;
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

 private:
  static constexpr std::uint32_t kEmpty = UINT32_MAX;  // pool slots never reach this value
  static constexpr std::size_t kNotFound = SIZE_MAX;

  struct Slot {
    OrderId key;
    std::uint32_t value = kEmpty;
  };

  static std::size_t round_up_pow2(std::size_t n) {
    std::size_t p = 1;
    while (p < n) p <<= 1;
    return p;
  }

  [[nodiscard]] std::size_t home(OrderId id) const noexcept {
    return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ULL) >> shift_);
  }

  [[nodiscard]] std::size_t locate(OrderId id) const noexcept {
    for (std::size_t i = home(id);; i = (i + 1) & mask_) {
      const Slot& s = slots_[i];
      if (s.value == kEmpty) return kNotFound;
      if (s.key == id) return i;
    }
  }

  void rehash(std::size_t capacity) {
    std::vector<Slot> old(capacity);
    old.swap(slots_);
    mask_ = capacity - 1;
    shift_ = 64U - static_cast<unsigned>(__builtin_ctzll(capacity));
    for (const Slot& s : old) {
      if (s.value == kEmpty) continue;
      std::size_t i = home(s.key);
      while (slots_[i].value != kEmpty) i = (i + 1) & mask_;
      slots_[i] = s;
    }
  }

  std::vector<Slot> slots_;
  std::size_t mask_ = 0;
  unsigned shift_ = 64;
  std::size_t size_ = 0;
};

}  // namespace lob

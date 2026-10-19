#pragma once

#include <cstddef>
#include <iosfwd>
#include <span>
#include <vector>

#include "lob/types.hpp"

namespace lob {

enum class EventType : std::uint8_t { Accepted, Trade, Cancelled, Rejected };

enum class RejectReason : std::uint8_t { None, ZeroQuantity, InvalidPrice, DuplicateId, UnknownOrder };

// One observable outcome of an engine operation. Field meaning by type:
//   Accepted   id, side, price (0 for market orders), qty
//   Trade      id = incoming order, resting_id, side = incoming side,
//              price = resting order's price, qty = traded quantity
//   Cancelled  id, side, price (0 for market orders), qty = quantity removed
//   Rejected   id, reason
struct Event {
  EventType type{};
  RejectReason reason = RejectReason::None;
  Side side = Side::Buy;
  OrderId id = 0;
  OrderId resting_id = 0;
  Price price = 0;
  Qty qty = 0;

  friend bool operator==(const Event&, const Event&) = default;
};

std::ostream& operator<<(std::ostream& os, const Event& e);

// Output buffer the engine appends to. Callers clear() it between operations;
// capacity is retained, so steady-state operation does not allocate.
class EventBuffer {
 public:
  explicit EventBuffer(std::size_t reserve = 64) { events_.reserve(reserve); }

  void accepted(OrderId id, Side side, Price price, Qty qty) {
    events_.push_back({EventType::Accepted, RejectReason::None, side, id, 0, price, qty});
  }
  void trade(OrderId id, OrderId resting_id, Side side, Price price, Qty qty) {
    events_.push_back({EventType::Trade, RejectReason::None, side, id, resting_id, price, qty});
  }
  void cancelled(OrderId id, Side side, Price price, Qty qty) {
    events_.push_back({EventType::Cancelled, RejectReason::None, side, id, 0, price, qty});
  }
  void rejected(OrderId id, RejectReason reason) {
    events_.push_back({EventType::Rejected, reason, Side::Buy, id, 0, 0, 0});
  }

  void clear() noexcept { events_.clear(); }
  [[nodiscard]] std::span<const Event> view() const noexcept { return events_; }
  [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }
  [[nodiscard]] bool empty() const noexcept { return events_.empty(); }
  [[nodiscard]] const Event& operator[](std::size_t i) const { return events_[i]; }
  [[nodiscard]] auto begin() const noexcept { return events_.begin(); }
  [[nodiscard]] auto end() const noexcept { return events_.end(); }

 private:
  std::vector<Event> events_;
};

}  // namespace lob

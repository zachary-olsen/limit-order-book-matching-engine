#include "lob/events.hpp"

#include <ostream>

namespace lob {

namespace {

const char* to_string(EventType t) {
  switch (t) {
    case EventType::Accepted: return "Accepted";
    case EventType::Trade: return "Trade";
    case EventType::Cancelled: return "Cancelled";
    case EventType::Rejected: return "Rejected";
  }
  return "?";
}

const char* to_string(RejectReason r) {
  switch (r) {
    case RejectReason::None: return "None";
    case RejectReason::ZeroQuantity: return "ZeroQuantity";
    case RejectReason::InvalidPrice: return "InvalidPrice";
    case RejectReason::DuplicateId: return "DuplicateId";
    case RejectReason::UnknownOrder: return "UnknownOrder";
  }
  return "?";
}

}  // namespace

std::ostream& operator<<(std::ostream& os, const Event& e) {
  os << to_string(e.type) << "{id=" << e.id;
  if (e.type == EventType::Rejected) return os << " reason=" << to_string(e.reason) << '}';
  os << ' ' << (e.side == Side::Buy ? "buy" : "sell");
  if (e.type == EventType::Trade) os << " resting=" << e.resting_id;
  return os << " px=" << e.price << " qty=" << e.qty << '}';
}

}  // namespace lob

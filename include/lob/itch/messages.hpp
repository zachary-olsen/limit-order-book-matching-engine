#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "lob/types.hpp"

// NASDAQ TotalView-ITCH 5.0 messages that affect the order book. Field
// offsets are relative to the message type byte; each message in a daily file
// is preceded by a 2-byte big-endian length. All integers are big-endian.
namespace lob::itch {

using Symbol = std::array<char, 8>;  // left-justified, space padded

// Common to every message: type(0) locate(1,2) tracking(3,2) timestamp(5,6).
inline constexpr std::size_t kHeaderLength = 11;

struct Header {
  std::uint64_t timestamp;  // nanoseconds since midnight
  std::uint16_t locate;     // per-day stock index
  std::uint16_t tracking;
  char type;
};

struct SystemEvent {  // 'S'
  Header h;
  char event_code;  // O, S, Q (market open), M (market close), E, C
};

struct StockDirectory {  // 'R'
  Header h;
  Symbol stock;
};

struct StockTradingAction {  // 'H'
  Header h;
  Symbol stock;
  char trading_state;  // H halted, P paused, Q quotation only, T trading
  std::array<char, 4> reason;
};

struct AddOrder {  // 'A', and 'F' (with MPID attribution, which we ignore)
  Header h;
  OrderId ref;
  Side side;
  Qty shares;
  Symbol stock;
  std::uint32_t price;
};

struct OrderExecuted {  // 'E'
  Header h;
  OrderId ref;
  Qty shares;
  std::uint64_t match;
};

struct OrderExecutedWithPrice {  // 'C'
  Header h;
  OrderId ref;
  Qty shares;
  std::uint64_t match;
  char printable;
  std::uint32_t price;
};

struct OrderCancel {  // 'X' (partial cancel)
  Header h;
  OrderId ref;
  Qty shares;
};

struct OrderDelete {  // 'D'
  Header h;
  OrderId ref;
};

struct OrderReplace {  // 'U': new id, same side, loses time priority
  Header h;
  OrderId old_ref;
  OrderId new_ref;
  Qty shares;
  std::uint32_t price;
};

struct Trade {  // 'P': execution against a hidden order, does not touch the visible book
  Header h;
  OrderId ref;
  Side side;
  Qty shares;
  Symbol stock;
  std::uint32_t price;
  std::uint64_t match;
};

// Lengths (including the type byte) of the messages we decode; 0 for others.
constexpr std::size_t decoded_length(char type) noexcept {
  switch (type) {
    case 'S': return 12;
    case 'R': return 39;
    case 'H': return 25;
    case 'A': return 36;
    case 'F': return 40;
    case 'E': return 31;
    case 'C': return 36;
    case 'X': return 23;
    case 'D': return 19;
    case 'U': return 35;
    case 'P': return 44;
    default: return 0;
  }
}

inline std::string to_string(const Symbol& s) {
  std::string out(s.data(), s.size());
  out.erase(out.find_last_not_of(' ') + 1);
  return out;
}

}  // namespace lob::itch

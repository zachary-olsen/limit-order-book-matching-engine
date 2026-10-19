#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "lob/itch/messages.hpp"

namespace lob::itch {

static_assert(std::endian::native == std::endian::little, "byte swapping assumes a little-endian host");

namespace detail {

// memcpy avoids unaligned-read UB; compilers lower it to a single load.
inline std::uint16_t be16(const std::uint8_t* p) noexcept {
  std::uint16_t v;
  std::memcpy(&v, p, sizeof v);
  return __builtin_bswap16(v);
}
inline std::uint32_t be32(const std::uint8_t* p) noexcept {
  std::uint32_t v;
  std::memcpy(&v, p, sizeof v);
  return __builtin_bswap32(v);
}
inline std::uint64_t be64(const std::uint8_t* p) noexcept {
  std::uint64_t v;
  std::memcpy(&v, p, sizeof v);
  return __builtin_bswap64(v);
}
inline std::uint64_t be48(const std::uint8_t* p) noexcept {
  return (std::uint64_t{be16(p)} << 32) | be32(p + 2);
}
inline Symbol symbol(const std::uint8_t* p) noexcept {
  Symbol s;
  std::memcpy(s.data(), p, s.size());
  return s;
}
inline Side side(std::uint8_t c) noexcept { return c == 'B' ? Side::Buy : Side::Sell; }

}  // namespace detail

// Default no-op callbacks. Handlers derive from this and define the callbacks
// they care about (each has a distinct name, so nothing is hidden).
// Optional extras detected at compile time:
//   void before_message(const Header&)  called for every well-formed message
//   bool stopped()                      checked before each message; true stops parsing
struct NullHandler {
  void on_system_event(const SystemEvent&) {}
  void on_stock_directory(const StockDirectory&) {}
  void on_add_order(const AddOrder&) {}
  void on_order_executed(const OrderExecuted&) {}
  void on_order_executed_with_price(const OrderExecutedWithPrice&) {}
  void on_order_cancel(const OrderCancel&) {}
  void on_order_delete(const OrderDelete&) {}
  void on_order_replace(const OrderReplace&) {}
  void on_trade(const Trade&) {}
  void on_other(const Header&) {}                // valid message we don't decode
  void on_malformed(char, std::size_t) {}        // wrong length for its type
};

// Decodes one message (starting at the type byte) and dispatches it.
// Returns false if the message is malformed.
template <class Handler>
bool parse_message(const std::uint8_t* p, std::size_t len, Handler& h) {
  using namespace detail;
  if (len < kHeaderLength) {
    h.on_malformed(len == 0 ? '\0' : static_cast<char>(p[0]), len);
    return false;
  }
  const char type = static_cast<char>(p[0]);
  const std::size_t expected = decoded_length(type);
  if (expected != 0 && len != expected) {
    h.on_malformed(type, len);
    return false;
  }
  const Header hd{be48(p + 5), be16(p + 1), be16(p + 3), type};
  if constexpr (requires { h.before_message(hd); }) h.before_message(hd);

  switch (type) {
    case 'S': h.on_system_event(SystemEvent{hd, static_cast<char>(p[11])}); break;
    case 'R': h.on_stock_directory(StockDirectory{hd, symbol(p + 11)}); break;
    case 'A':
    case 'F':
      h.on_add_order(AddOrder{hd, be64(p + 11), side(p[19]), be32(p + 20), symbol(p + 24), be32(p + 32)});
      break;
    case 'E': h.on_order_executed(OrderExecuted{hd, be64(p + 11), be32(p + 19), be64(p + 23)}); break;
    case 'C':
      h.on_order_executed_with_price(OrderExecutedWithPrice{
          hd, be64(p + 11), be32(p + 19), be64(p + 23), static_cast<char>(p[31]), be32(p + 32)});
      break;
    case 'X': h.on_order_cancel(OrderCancel{hd, be64(p + 11), be32(p + 19)}); break;
    case 'D': h.on_order_delete(OrderDelete{hd, be64(p + 11)}); break;
    case 'U': h.on_order_replace(OrderReplace{hd, be64(p + 11), be64(p + 19), be32(p + 27), be32(p + 31)}); break;
    case 'P':
      h.on_trade(Trade{hd, be64(p + 11), side(p[19]), be32(p + 20), symbol(p + 24), be32(p + 32), be64(p + 36)});
      break;
    default: h.on_other(hd); break;
  }
  return true;
}

// Parses length-prefixed messages from a buffer. Stops at the first incomplete
// message (or when the handler reports stopped()) and returns the number of
// bytes consumed, so the caller can carry the tail into the next chunk.
template <class Handler>
std::size_t parse_buffer(const std::uint8_t* data, std::size_t size, Handler& h) {
  std::size_t pos = 0;
  while (size - pos >= 2) {
    if constexpr (requires { h.stopped(); }) {
      if (h.stopped()) break;
    }
    const std::size_t len = detail::be16(data + pos);
    if (size - pos - 2 < len) break;
    parse_message(data + pos + 2, len, h);
    pos += 2 + len;
  }
  return pos;
}

}  // namespace lob::itch

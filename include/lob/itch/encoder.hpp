#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "lob/itch/messages.hpp"

namespace lob::itch {

// Builds length-prefixed ITCH 5.0 messages, for tests and synthetic benchmarks.
class Encoder {
 public:
  void system_event(std::uint16_t locate, std::uint64_t ts, char code) {
    begin('S', locate, ts, 12);
    put8(static_cast<std::uint8_t>(code));
  }

  void stock_directory(std::uint16_t locate, std::uint64_t ts, std::string_view stock) {
    begin('R', locate, ts, 39);
    put_symbol(stock);
    put8('Q');      // market category
    put8('N');      // financial status
    put32(100);     // round lot size
    put8('N');      // round lots only
    put8('C');      // issue classification
    put_text("Z ");  // issue subtype
    put8('P');      // authenticity
    put8('N');      // short sale threshold
    put8('N');      // IPO flag
    put8('1');      // LULD tier
    put8('N');      // ETP flag
    put32(0);       // ETP leverage
    put8('N');      // inverse
  }

  void add_order(std::uint16_t locate, std::uint64_t ts, OrderId ref, Side side, Qty shares,
                 std::string_view stock, std::uint32_t price) {
    begin('A', locate, ts, 36);
    put_add_body(ref, side, shares, stock, price);
  }

  void add_order_mpid(std::uint16_t locate, std::uint64_t ts, OrderId ref, Side side, Qty shares,
                      std::string_view stock, std::uint32_t price, std::string_view mpid) {
    begin('F', locate, ts, 40);
    put_add_body(ref, side, shares, stock, price);
    put_text(mpid);
  }

  void order_executed(std::uint16_t locate, std::uint64_t ts, OrderId ref, Qty shares, std::uint64_t match) {
    begin('E', locate, ts, 31);
    put64(ref);
    put32(shares);
    put64(match);
  }

  void order_executed_with_price(std::uint16_t locate, std::uint64_t ts, OrderId ref, Qty shares,
                                 std::uint64_t match, char printable, std::uint32_t price) {
    begin('C', locate, ts, 36);
    put64(ref);
    put32(shares);
    put64(match);
    put8(static_cast<std::uint8_t>(printable));
    put32(price);
  }

  void order_cancel(std::uint16_t locate, std::uint64_t ts, OrderId ref, Qty shares) {
    begin('X', locate, ts, 23);
    put64(ref);
    put32(shares);
  }

  void order_delete(std::uint16_t locate, std::uint64_t ts, OrderId ref) {
    begin('D', locate, ts, 19);
    put64(ref);
  }

  void order_replace(std::uint16_t locate, std::uint64_t ts, OrderId old_ref, OrderId new_ref, Qty shares,
                     std::uint32_t price) {
    begin('U', locate, ts, 35);
    put64(old_ref);
    put64(new_ref);
    put32(shares);
    put32(price);
  }

  void trade(std::uint16_t locate, std::uint64_t ts, OrderId ref, Side side, Qty shares, std::string_view stock,
             std::uint32_t price, std::uint64_t match) {
    begin('P', locate, ts, 44);
    put_add_body(ref, side, shares, stock, price);
    put64(match);
  }

  // A message of an arbitrary type and length: the header zero-padded, or
  // truncated if length is shorter than a header.
  void raw(char type, std::uint16_t locate, std::uint64_t ts, std::uint16_t length) {
    const std::size_t body = bytes_.size() + 2;
    begin(type, locate, ts, length);
    bytes_.resize(body + length, 0);
  }

  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }
  void clear() noexcept { bytes_.clear(); }

 private:
  void begin(char type, std::uint16_t locate, std::uint64_t ts, std::uint16_t length) {
    put16(length);
    put8(static_cast<std::uint8_t>(type));
    put16(locate);
    put16(0);  // tracking number
    put16(static_cast<std::uint16_t>(ts >> 32));
    put32(static_cast<std::uint32_t>(ts));
  }

  void put_add_body(OrderId ref, Side side, Qty shares, std::string_view stock, std::uint32_t price) {
    put64(ref);
    put8(side == Side::Buy ? 'B' : 'S');
    put32(shares);
    put_symbol(stock);
    put32(price);
  }

  void put8(std::uint8_t v) { bytes_.push_back(v); }
  void put16(std::uint16_t v) {
    put8(static_cast<std::uint8_t>(v >> 8));
    put8(static_cast<std::uint8_t>(v));
  }
  void put32(std::uint32_t v) {
    put16(static_cast<std::uint16_t>(v >> 16));
    put16(static_cast<std::uint16_t>(v));
  }
  void put64(std::uint64_t v) {
    put32(static_cast<std::uint32_t>(v >> 32));
    put32(static_cast<std::uint32_t>(v));
  }
  void put_text(std::string_view s) {
    for (char c : s) put8(static_cast<std::uint8_t>(c));
  }
  void put_symbol(std::string_view s) {
    for (std::size_t i = 0; i < 8; ++i) put8(i < s.size() ? static_cast<std::uint8_t>(s[i]) : ' ');
  }

  std::vector<std::uint8_t> bytes_;
};

}  // namespace lob::itch

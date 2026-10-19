#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "lob/itch/parser.hpp"

struct gzFile_s;

namespace lob::itch {

// Streaming reader over a gzip (or plain) file via zlib; never decompresses to disk.
class GzReader {
 public:
  explicit GzReader(const std::string& path);
  ~GzReader();
  GzReader(const GzReader&) = delete;
  GzReader& operator=(const GzReader&) = delete;

  // Reads up to n bytes; returns 0 at end of file. Throws on I/O errors.
  std::size_t read(std::uint8_t* dst, std::size_t n);

 private:
  gzFile_s* file_;
};

inline constexpr std::size_t kDefaultChunk = std::size_t{4} << 20;

// Streams every message in the file through the handler. Returns the number of
// bytes parsed. Throws if the file ends mid-message.
template <class Handler>
std::uint64_t stream_file(GzReader& reader, Handler& h, std::size_t chunk = kDefaultChunk) {
  std::vector<std::uint8_t> buf(chunk);
  std::size_t have = 0;
  std::uint64_t parsed = 0;
  for (;;) {
    if constexpr (requires { h.stopped(); }) {
      if (h.stopped()) return parsed;
    }
    const std::size_t n = reader.read(buf.data() + have, buf.size() - have);
    if (n == 0) break;
    have += n;
    const std::size_t used = parse_buffer(buf.data(), have, h);
    if (used == 0 && have == buf.size()) throw std::runtime_error("ITCH message larger than read buffer");
    std::memmove(buf.data(), buf.data() + used, have - used);
    have -= used;
    parsed += used;
  }
  if (have != 0) throw std::runtime_error("ITCH file ends in the middle of a message");
  return parsed;
}

// Decompresses up to max_messages length-prefixed messages into memory, so
// benchmarks can measure parsing and book updates without I/O.
inline std::vector<std::uint8_t> load_messages(GzReader& reader, std::uint64_t max_messages,
                                               std::uint64_t& loaded) {
  std::vector<std::uint8_t> out;
  std::vector<std::uint8_t> buf(kDefaultChunk);
  std::size_t have = 0;
  loaded = 0;
  while (loaded < max_messages) {
    const std::size_t n = reader.read(buf.data() + have, buf.size() - have);
    if (n == 0) break;
    have += n;
    std::size_t pos = 0;
    while (loaded < max_messages && have - pos >= 2) {
      const std::size_t frame = 2 + detail::be16(buf.data() + pos);
      if (have - pos < frame) break;
      pos += frame;
      ++loaded;
    }
    out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(pos));
    std::memmove(buf.data(), buf.data() + pos, have - pos);
    have -= pos;
  }
  return out;
}

}  // namespace lob::itch

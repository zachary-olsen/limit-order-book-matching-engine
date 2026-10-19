#include <zlib.h>

#include <climits>

#include "lob/itch/file.hpp"

namespace lob::itch {

GzReader::GzReader(const std::string& path) : file_(gzopen(path.c_str(), "rb")) {
  if (file_ == nullptr) throw std::runtime_error("cannot open " + path);
  gzbuffer(file_, 1U << 20);
}

GzReader::~GzReader() { gzclose(file_); }

std::size_t GzReader::read(std::uint8_t* dst, std::size_t n) {
  const auto want = static_cast<unsigned>(n > INT_MAX ? INT_MAX : n);
  const int got = gzread(file_, dst, want);
  if (got < 0) {
    int code = 0;
    throw std::runtime_error(std::string("gzread failed: ") + gzerror(file_, &code));
  }
  return static_cast<std::size_t>(got);
}

}  // namespace lob::itch

#pragma once

#include <gtest/gtest.h>

#include <string>

#include "lob/map_order_book.hpp"
#include "lob/pooled_order_book.hpp"

namespace lob::testing {

// Every book implementation; typed tests run against all of them.
using AllBooks = ::testing::Types<MapOrderBook, PoolOrderBook, PoolFlatOrderBook, VectorOrderBook>;

template <class Book>
constexpr const char* book_name();
template <>
constexpr const char* book_name<MapOrderBook>() { return "Map"; }
template <>
constexpr const char* book_name<PoolOrderBook>() { return "Pool"; }
template <>
constexpr const char* book_name<PoolFlatOrderBook>() { return "PoolFlat"; }
template <>
constexpr const char* book_name<VectorOrderBook>() { return "Vector"; }

struct BookNames {
  template <class Book>
  static std::string GetName(int) {
    return book_name<Book>();
  }
};

}  // namespace lob::testing

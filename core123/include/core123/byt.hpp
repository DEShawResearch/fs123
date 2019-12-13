// Based on our "success" with str_view, we try to invent
// core123::byt which is a std::byte when the implementation
// permits, and an unsigned char otherwise.  Fingers crossed...

#pragma once

#include <cstddef>
#if __has_include(<version>)
#include <version>
#endif

namespace core123{
#if __cpp_lib_byte >= 201603L
using byt=std::byte;
#else
using byt=unsigned char;
#endif
}

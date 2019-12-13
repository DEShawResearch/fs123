#pragma once

// It's way too much to type std::experimental::string_view over and
// over.  AND it's going to become std::string_view in the pretty near
// future.  This header provides core123::str_view and
// core123::basic_str_view which are easier to type, and which we
// should be able to easily swing over to non-experimental when the
// time comes.

// Ha! ...  "easily...when the time comes"

// llvm's libc++ and gnu's libstdc++ have different and almost comically
// incompatible approaches to the experimental namespace.  Let's compare
// llvm-8.0.0 and gcc-8.1.0
//
// - llvm has <experimental/string_view>, but all it contains
//   is an #error telling you to use <string_view> instead.
// - gnu  has <string_view>, but if __cplusplus<201703L,
//   it's #if'ed down to no content.
// - gnu's <string_view> defines __cpp_lib_string_view if it
//   actually implements it.
// - llvm doesn't define __cpp_lib_string_view anywhere - neither
//   in <string_view>, nor in <version>.
// - gnu doesn't have <version> (until gcc-9)

// That seems to leave us with no viable set of conditionals based
// on just  __has_include() and __cpp_lib_string_view.  We're compelled
// to rely on implementation-specific knowledge:
//
// So we rely on the apparent convention that llvm's <string_view>
// defines _LIBCPP_STRING_VIEW

#if __has_include(<string_view>)
#include <string_view>
#endif
#if __has_include(<version>)
#include <version>
#endif

#if __cpp_lib_string_view >= 201603 || defined(_LIBCPP_STRING_VIEW)

namespace core123{
template <typename CharT, typename Traits>
using basic_str_view = std::basic_string_view<CharT, Traits>;
using str_view = basic_str_view<char, std::char_traits<char>>;

} // namespace core123

#else

#include <experimental/string_view>

namespace core123{
template <typename CharT, typename Traits>
using basic_str_view = std::experimental::basic_string_view<CharT, Traits>;
using str_view = basic_str_view<char, std::char_traits<char>>;

} // namespace core123
#endif


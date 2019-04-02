#pragma once

#include "str_view.hpp"
#include "scanint.hpp"
#include "svstream.hpp"
#include "unused.hpp"
#include <string>
#include <limits>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <tuple>
#include <utility>

// svto is intended for "one-off" use where the caller doesn't care
// how many bytes were "consumed" and it's convenient to explicitly
// name the type of the value expected.

// svscan is intended for iterated usage, facilitating deserialization
// of several values from a single string_view.

//
// template <typename Type>
//   Type svto(str_view sv, size_t start=0)
//
// Tries to interpret the characters starting at sv[start] as optional
// whitespace followed by a string representation of Type.  Returns
// the value or throws an exception if no conversion is possible.
// Also throws an exception if sv contains non-whitespace after
// the converted characters.  Note that svto<std::string>(sv, start)
// throws if there are no non-whitespace characters between sv[start]
// and sv[sv.size()] because the stream extraction operator into
// a std::string fails if there are no non-whitespace characters to
// extract.  This makes it impossible for svto to return an empty
// string.

// template <typename Type>
// size_t
//      svscan(str_view sv, Type *p, size_t start=0)
//
// If &sv[start] starts with optional white-space followed by
// characters that represent a value of type T then assign the
// represented value to *p and return the offset in the original sv of
// the character following the last one used to represent the value.
//
// If T is a floating point type, then the extraction is done
// with strtof, strtod or strtold, which "do the right thing" with
// NaN, Inf, etc.
//
// If T is an integral type, then the extraction is done with
// scanint<T>, which is much faster than using an istream.
//
// If T is a tuple of references (c.f. std::tie), then each of the the
// members of the tuple is svscan'ed in order.  The character
// immediately following each represented value is ignored (so that
// any single-character is viable as a delimiter).
//
// An overload of svscan is intended for use on input ranges:
//
// template <class IITER>
// ssize_t
//      svscan(str_view sv, IITER b,  IITER e, ize_t start=0)
//
// will scan values from sv into successive values (&*b++) in the range [b, e).
//
// It is permitted/encouraged for developers to provide overloads
// for user-defined types, e.g.,
//
//     sizet svscan(str_view sv, Udt* p, size_t start=0);
//
// If T is any other type, then T must be Default Constructible
// and there must be an overloaded extraction operator for T, e.g.,
//     std::istream& operator>>(std::istream&, T&);
// in which case the value of T is obtained using the extraction
// operator.  The istream will have its unsetf(std::ios::basefield )
// method called prior to any extraction, which has the effect of
// making integer extractions behave as if by "%i".  I.e.,
// "013", "11" and "0xb" will all be converted to the number 11.
//
// Data copies are generally avoided.  The time and space complexity
// are:
//
//   O(returned_value- start)
//
// and not, for example, O(sv.size()).  This makes it practical to call
// svscan to extract values repeatedly, one-at-a-time from a
// large string_view.
//
// N.B.  Despite the avoidance of copies, constructing an isvstream
// takes ~150ns on a 2012 3.5GHz Haswell.  "A nanosecond here, a
// nanosecond there, pretty soon you're talking real overhead."
//
// To facilitate end-of-string detection, there is an overload
// for:
//     svscan<nullptr_t>(string_view sv, size_t start)
// which skips whitespace.  I.e., it returns a pair:
//    {nullptr, offset-of-next-non-whitespace}
//
// Unlike stringTo, svscan<std::string> follows conventional istream
// extraction rules.  I.e., it scans for a whitespace-delimited
// string.

// Examples:
//      // simplest possible use-case
//      str_view sv  = " 19 xyzzy";
//      int i;
//      auto nxt = svscan(sv, &i); // i=19, nxt = 3
//    
//      // in a loop:
//      sv = "1 2 3 4 5 6 7 8 unexpected ";
//      int a[8];
//      nxt = 0;
//      for(int i=0; i<8; ++i){
//        nxt = svscan(sv, &a[i], nxt);
//      }
//      if(svscan(sv, nullptr, nxt) != sv.size())
//         std::cerr << "There's stuff after the eighth integer!\n";
//
// Note that there are implicit conversions from char* and
// std::string& string_view, so svscan may be called with a
// char pointer or a string reference as its first argument.  Note,
// though that the conversion from char* requires runtime
// determination of the string's length, which can be costly for very
// long strings.  If the intent is to iterate through a long char*
// buffer with svscan, then a single invocation of the
// string_view constructor outside the loop will entail less overhead.

namespace core123{
// First, a non-templated overload for the nullptr_t case.  If this
// results in ambiguity, find another way to ask for the whitespace count.
inline size_t
svscan(str_view sv, std::nullptr_t, size_t start=0){
    auto nonwhite = sv.find_first_not_of(" \n\t\f\r", start);
    return nonwhite!=sv.npos ? nonwhite : sv.size();
}


template <typename Type>
typename std::enable_if<!std::is_floating_point<Type>::value &&
                        !std::is_integral<Type>::value,
                        size_t>::type
svscan(str_view sv, Type* vp, size_t start=0){
    using namespace std;
    if(sv.size() < start)
        throw invalid_argument("svscan<T>:  len<start");
    isvstream is(sv.substr(start));
    is.unsetf(std::ios::basefield);
    is >> *vp;    // Type must have an istream extraction operator>>
    if(!is)
        throw invalid_argument("svscan<T>:  stream extraction operator>> failed");
    return start + is.tellg();
}

template <typename Type>
typename std::enable_if<std::is_integral<Type>::value, size_t>::type
svscan(str_view sv, Type* vp, size_t start=0){
    return scanint<Type>(sv, vp, start);
}

// Do the floating point types with strto{f,d,ld}
template <typename Ftype>
typename std::enable_if<std::is_floating_point<Ftype>::value,
                        size_t>::type
svscan(str_view sv, Ftype* vp, size_t start=0){
    using namespace std;
    static_assert( std::is_unsigned<decltype(sv.size())>::value, "sv.size() must be unsigned");

    // We'd really like to just call strtof(sv.data()) , but there's
    // no guarantee that a string_view is NUL-terminated, so that's a
    // very bad idea.

    // Premature optimization? - Only copy non-white characters
    // into the string that we pass to strtox.
    auto beg = std::min(sv.find_first_not_of(" \n\t\f\r", start), sv.size());
    // N.B.  we're prepared to accept nan and infinity (mixed case), but
    // we're going to draw the line at NaN(alnum) where alnum is any arbitrary
    // alphanumeric sequence.
    auto en = sv.find_first_not_of("0123456789abcdefABCDEFxXpP.+-NnIiTtYy", beg);
    errno = 0;
    std::string s(sv.substr(beg, en - beg));
    Ftype x;
    char *endp;
    if( std::is_same<Ftype, float>::value )
        x = ::strtof(s.c_str(), &endp);
    else if( std::is_same<Ftype, double>::value )
        x = ::strtod(s.c_str(), &endp);
    else
        x = ::strtold(s.c_str(), &endp);
    if(errno == EINVAL)
        throw invalid_argument("svscan<Ftype> argument \"" + s + "\" is invalid");
    *vp = x;
    return beg+(endp - s.c_str());
}

// svscan for a range:
template<class IITER>
size_t svscan(str_view sv, IITER b, IITER e, size_t start = 0){
    while(b != e)
        start = svscan(sv, &*b++, start);
    return start;
}

// svscan for ties (tuples of references).  So you can say:
//    <declarations of a, b, c>;
//    start = svscan(sv, std::tie(a, b, c), start);
// 
template <class Tuple, std::size_t ... Is>
size_t svscan_tuple(str_view sv, const Tuple& t, size_t start, std::index_sequence<Is...>){
#if __cpp_fold_expressions // c++17 
    // expand pack over the binary comma expression
    ((start = svscan(sv, &std::get<Is>(t), start)+1), ... );
#else    
    // expand into an unused array-inializer.
    size_t _[] = {(start = svscan(sv, &std::get<Is>(t), start)+1) ... }; unused(_);
#endif
    return start;
}

template <typename ... Types>
size_t svscan(str_view sv, const std::tuple<Types& ...>& t, size_t start=0){
    return svscan_tuple(sv, t, start, std::index_sequence_for<Types...>{});
}

// svto<T> is the "traditional" style, returning a T.
template <typename T>
T svto(str_view sv, size_t start=0){
    T ret;
    auto nxt = svscan<T>(sv, &ret, start);
    if(nxt != sv.size() && svscan(sv, nullptr, nxt) != sv.size())
        throw std::invalid_argument("svto: non-whitespace following converted string: " + std::string(sv.substr(nxt, 30)));
    return ret;
}
} // namespace core123

// Read and write data serialized in the 'CDB Record Format'.
//
// CDB Record Format is a succinct, human-friendly, easy-to-parse
// format that is particularly well suited to serializing lists of
// key-value pairs.  Keys and values can be of arbitrary size and may
// contain arbitrary characters (i.e., NUL and non-ascii values are
// fine.)
//
// The CDB Record Format was created by Dan Bernstein for his cdbmake
// program.  See https://cr.yp.to/cdb/cdbmake.html
//
// The implementation here is completely independent.
//
// Each record a newline-terminated string of the form:
//
//     +klen,dlen:key->data
//
// E.g.
//
//    +8,11:greeting->hello world
//
// The +, comma, colon, -> and terminal newline characters are
// exact literal constants and are required to be exactly as
// shown with no additional whitespace or padding.  The klen
// and dlen are decimal (base 10) integers.  The key and data
// are exactly klen and dlen bytes in length.
//
// The last record is indicated by an extra newline.
//
// The following functions are defined:
//
// size_t
// svscan_crf(str_view in, std::function<void(str_view, str_view)> f, size_t start=0);
//
//   Parse the str_view 'in', starting at offset 'start', assuming it
//   consists of a sequence of CDB Records, followed by a newline.
//   After each record is parsed, the callback function f(key, data)
//   is called.
//
//   The offset of the character following the extra newline that ends
//   the sequence is returned.
//
//   If any errors are detected, an object derived from std::exception
//   is thrown.  Note that the callback, f, may be called one or more
//   times before an error is detected and an exception is thrown.

// template <typename Iter>
// std::string format_crf(Iter b, Iter e);
//
//   Formats a CDB from a range of pairs of string-like objects.
//   Iter's value_type must be a pair of things that have a size()
//   member and that are convertible into std::string.  E.g.,
//
//      vector<pair<str_view, str_view>> v;
//      s = format_crf(v.begin(), v.end());
//
// template <typename  Container>
// inline std::string format_crf(const Container& c);
//
//   A convenience function equivalent to:
//         format_crf(std::begin(c), std::end(c));

#include <core123/str_view.hpp>
#include <core123/scanint.hpp>
#include <functional>
#include <vector>
#include <utility>
#include <string>
#include <stdexcept>
#include <exception>

namespace core123{

namespace detail{
inline size_t
expect(str_view in, char c, size_t where){
    // N.B.  may throw out_of_range or runtime_error
    if(in.at(where) != c)
        throw std::runtime_error(std::string("crf.hpp: detail::expect(str_view, c='") + c + "') got '" + in.at(where) + "'");
    return where+1;
}

inline size_t
svscan_crf_onerecord(str_view in, std::function<void(str_view, str_view)> f, size_t start=0){
    auto next = start;
    next = expect(in, '+', next);
    size_t klen, dlen;
    next = scanint<size_t, 10, false>(in, &klen, next);
    next = expect(in, ',', next);
    next = scanint<size_t, 10, false>(in, &dlen, next);
    next = expect(in, ':', next);
    auto k = in.substr(next, klen);
    if( k.size() != klen )
        throw std::out_of_range("klen too large");
    next += klen;
    next = expect(in, '-', next);
    next = expect(in, '>', next);
    auto d = in.substr(next, dlen);
    if( d.size() != dlen )
        throw std::out_of_range("dlen too large");
    next += dlen;
    next = expect(in, '\n', next);
    f(k, d);
    return next;
}
} // namespace detail

inline size_t
svscan_crf(str_view in, std::function<void(str_view, str_view)> f, size_t start=0) try{
    auto next = start;
    while( in.at(next) != '\n' )
        next = detail::svscan_crf_onerecord(in, f, next);
    return next+1;
 }catch(std::runtime_error& re){
    std::throw_with_nested(std::runtime_error("in svscan_crf"));
 }

template <typename Iter>
inline std::string
format_crf(Iter b, Iter e){
    std::string s;
    for(; b!=e; ++b)
        s += '+' + std::to_string(b->first.size()) + ',' + std::to_string(b->second.size()) + ':' + std::string(b->first) + "->" + std::string(b->second) + '\n';
    s += '\n';
    return s;
}

template <typename  Container>
inline std::string
format_crf(const Container& c){
    return format_crf(std::cbegin(c), std::cend(c));
}
} // namespace core123

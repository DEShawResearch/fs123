#pragma once
// various convenient string handling utilities

#include <core123/str_view.hpp>
#include <core123/streamutils.hpp>
#include <iomanip>
#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <sstream>
#include <cctype>

namespace core123 {

// CILess - comparison class that can be used for case-insensitive
// comparisons, uses strcasecmp.  e.g.
//
//    std::map<std::string,std::string,core123::CILess> foo;
//
// creates a map foo that will ignore the case of keys.  Note that the
// original keys (with case intact) are stored in the map, so
// iterators will return a pair with .first in the original case
class CILess {
public:
    bool operator()( str_view lhs, str_view rhs ) const {
        auto lhs_shorter = lhs.size() < rhs.size();
        auto minsz = lhs_shorter ? lhs.size() : rhs.size();
        auto r = ::strncasecmp(lhs.data(), rhs.data(), minsz);
        return r<0 || (lhs_shorter && r==0);
    }
};

// endswith,startswith,lstrip,rstrip - basic string utiltities that are trickier than
// they first appear.

inline bool endswith(str_view s, str_view suf){
    if( suf.size() > s.size() )
        return false;
    return s.compare(s.size()-suf.size(), suf.size(), suf)==0;
}

inline bool startswith(str_view s, str_view pfx){
    if( pfx.size() > s.size() )
        return false;
    return s.compare(0, pfx.size(), pfx)==0;
}

inline str_view sv_rstrip(str_view s){
    auto i = s.find_last_not_of(" \r\n\t\f\v");
    if (i == s.npos)
        return "";
    return s.substr(0, i+1);
}

inline str_view sv_lstrip(str_view s){
    auto i = s.find_first_not_of(" \r\n\t\f\v");
    if (i == s.npos)
        return "";
    return s.substr(i);
}

inline str_view sv_strip(str_view s){
    return sv_lstrip(sv_rstrip(s));
}

inline std::string lstrip(str_view s) { return std::string(sv_lstrip(s)); }
inline std::string rstrip(str_view s) { return std::string(sv_rstrip(s)); }
inline std::string strip(str_view s) { return std::string(sv_strip(s)); }

// Random formatting tasks:  quoted printable, urlescape, etc.
// converts string view to a quoted-printable string
// can be decoded with Python binascii.a2b_qp
// XXX we arbitrarily decide not to pass TABs through since
// it might make lines display longer than 80 col.
inline std::string quopri(str_view s){
    static const char hexchars[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
    std::string ret;
    size_t i, iline;
    for (i = iline = 0; i < s.size(); i++) {
        auto c = s[i];
        if ((c >= 33 && c <= 126 && c != 61) ||
            (c == 32 && i != (s.size()-1))) {
            if (iline == 74) {
                ret += "=\n";
                iline = 0;
            }
            ret += c;
            iline++;
        } else {
            if (iline >= 72) {
                ret += "=\n";
                iline = 0;
            }
            ret += '=';
            ret += hexchars[((unsigned)c & 0xf0) >> 4];
            ret += hexchars[((unsigned)c & 0x0f)];
            iline += 3;
        }
    }
    return ret;
}

// There's a curl_easy_escape, but it transforms *everything*, including /.
// We don't want that, and really, it's not that hard...
inline std::string urlescape(str_view in){
    std::string ret;
    ret.reserve(3*in.size());
    for(auto c : in){
        if( ( c >= 'a' && c <= 'z' ) ||
            ( c >= 'A' && c <= 'Z' ) ||
            ( c >= '0' && c <= '9' ) ||
            c=='-' || c=='.' || c=='_' || c=='~' ||
            c== '/' ) // !! '/' is NOT normally allowed in a urlencoded string.  This is intentional!
            ret.push_back(c);
        else{
            const char digits[] = "0123456789ABCDEF";
            ret.push_back('%');
            ret.push_back(digits[(c>>4)&0xf]);
            ret.push_back(digits[(c   )&0xf]);
        }
    }
    return ret;
}

template <typename T> inline std::string tohex(T u, bool zeropad = true)
{
    static unsigned char hexchars[16] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };
    char s[sizeof(u)*2], *cp = s + sizeof(s) - 1;
    
    size_t i = 0;
    while (i < sizeof(s)) {
        unsigned c = u & 0xF;
        u >>= 4;
        *cp-- = hexchars[c] & 0xFF;
        //printf("%02X %llx\n", *(cp+1), u);
        i++;
        if (!zeropad && u == 0) break;
    }
    return std::string(s+sizeof(s)-i, i);
}

inline std::string hexdump(str_view s, bool alwayshex = false, const char *sep = " ") {
    std::string r;
    for (const auto c : s) {
        if (!alwayshex && isprint(c)) {
            r += sep;
            r += ' ';
            r += c;
        } else {
            r += sep + tohex(c);
        }
    }
    return r;
}

// svsplit_exact - split a str_view, s, into a vector of str_views by
// some delimiter, d.  Only characters in s at positions greater than
// or equal to start are considered.  If start is greater than
// s.size(), an empty vector is returned.
//
// The delimiter must be at least one character in length, and must
// match a substring of s, exactly, in its entirety in order to split
// the output.
//
// odd/surprising corner cases:
//   it's an error if delim is empty
//
//   the vector returned has zero length if and only if start > s.size().
//
//   if s[start] starts with delim, the first element of the returned
//         vector is the empty string
//
//   if the delimiter appears more than once after s[start] with no
//         intervening characters, then empty strings are inserted
//         into the output.
//
//   if s ends with delim, the last element of the returned vector
//         is the empty string.
inline auto svsplit_exact(str_view s, str_view delim, size_t start = 0)
{
    std::vector<core123::str_view> v;
    if(delim.empty())
        throw std::invalid_argument("svsplit_exact delim must be non-empty");
    while(start <= s.size()){
        auto next = s.find(delim, start);
        v.push_back(s.substr(start, next-start));
        if(next == str_view::npos)
            break;
        start = next+delim.size();
    }
    return v;
}

// svsplit_any - split a str_view into a vector of str_views by one
// or more characters in the delimiter.  Only characters in s
// at positions greater than or equal to start are considered.
// If start is greater than s.size(), an empty vector is returned.
//
// odd/surprising corner cases:
//   the vector returned has zero length if and only if start  > s.size().
//
//   if delim is empty and start <= s.size(), the returned vector has
//         one element consisting of s.substr(start).
//
//   if s[start] starts with characters from delim, the first element
//         of the returned vector is the empty string
//
//   if s ends with characters from delim, the last element of the
//         returned vector is the empty string.
//
//   if s consists entirely of characters from delim, the vector
//         returned has length 2, containing two empty strings.
inline auto svsplit_any(str_view s, str_view delim, size_t start = 0)
{
    std::vector<core123::str_view> v;
    while(start <= s.size()){
        auto nextdelim = s.find_first_of(delim, start);
        v.push_back(s.substr(start, nextdelim-start));
        start = s.find_first_not_of(delim, nextdelim);
    }
    return v;
}

// conveniently encode a cstring for printing using C literal conventions
inline std::string cstr_encode(const char *s) {
    std::string ret;
    int c;
    while ((c = *s++) != '\0') {
        c &= 0xff; // avoid sign extension problems!
        if (isgraph(c)) {
            ret.push_back(c);
        } else {
            ret += "\\x" + tohex(static_cast<unsigned char>(c));
        }
    }
    return ret;
}

/* direct-to-string versions of the 'ins' function family in streamutils.hpp

  stringifiers: str, strbe, strtuple 
     Return a string.  Internally, they create an ostringstream,
     forward their arguments to the corresponding inserter, and then
     return the result of the ostringstream's str() method.  E.g.,

     str(a, b, 3, 3.1415, some_udt);
     strbe(begin(v), end(v));
     "[" + strbe(", ", v) + "]"
     "(" + instuple(", ", t) + ")"


  formatters: fmt, vfmt
     Format their arguments according to a printf-style format string.
     [v]fmt is a replacement for [v]stringprintf.  E.g., 

     fmt("%zd.%09d", ts.tv_sec, ts.tv_nsec)

  See also 'strfunargs' in throwutils.hpp.
*/

template <typename ... Types>
auto
str_sep(const char *sep, Types const& ... values){
    std::ostringstream oss;
    oss << ins_sep(sep,  values ... );
    return oss.str();
}

template <typename ... Types>
auto
str(Types const& ... values){
    return str_sep(" ", values ...);
}

template <typename ... Types>
auto
strtuple(const char *sep, const std::tuple<Types...>& t){
    std::ostringstream oss;
    oss << instuple_sep(sep, t);
    return oss.str();
}

template <typename ... Types>
auto
strtuple(const std::tuple<Types...>& t){
    return strtuple(" ", t);
}

template <typename ITER>
auto
strbe(const char *sep, ITER b, ITER e){
    std::ostringstream oss;
    oss << insbe(sep, b, e);
    return oss.str();
}

template <typename ITER>
auto
strbe(ITER b, ITER e){
    return strbe(" ", b, e);
}

template <typename COLL>
auto
strbe(const char *sep, const COLL& coll){
    return strbe(sep, std::begin(coll), std::end(coll));
}

template <typename COLL>
auto
strbe(const COLL& coll){
    return strbe(" ", coll);
}

inline std::string
vfmt(const char *fmt, va_list va){
    size_t plen = 512;
    va_list ap;
    bool retried = false;
    std::unique_ptr<char[]> p;

 retry:
    p.reset(new char[plen]);
    va_copy(ap, va);
    auto n = vsnprintf(p.get(), plen, fmt, ap);
    va_end(ap);
    if(n<0)
        throw std::runtime_error("vsnprintf returned negative in vstringprintf");
    if(size_t(n)>=plen){
        if(retried)
            throw std::runtime_error("vsnprintf lied to vstringprintf about how much space it would need");
        retried = true;
        // vsnprintf returns the number of chars it would have written.  It's
        // been that way since 1999 (glibc 2.1).
        plen = n+1;
        goto retry;
    }
    return {p.get(), size_t(n)};
}

inline std::string fmt(const char  *fmt, ...)
    __attribute__ ((__format__ (__printf__, 1, 2)));
inline std::string fmt(const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    auto ret = vfmt(fmt, ap);
    va_end(ap);
    return ret;
}    

// nanos - a class that contains a long long count of billionths, with
//   an operator<< that formats the value as [-]WWW.FFFFFFFFF with
//   exactly 9 decimal digits to the right of the decimal point.
//
//   nanos can be constructed from a long long and has a conversion
//   operator back to long long.
//
//   Thus, you can say:
//
//    long long ll = 31415926535;
//
//    os << nanos(ll) << "\n";  // -> 31.415962535
//
struct nanos{
    static constexpr unsigned long long billion_ull = 1000000000ull;
    static constexpr long long billion_ll = 1000000000ll;
    long long cnt;

    nanos(long long _cnt) : cnt(_cnt){}
    operator long long int () const{
        return cnt;
    }
    friend std::ostream& operator<<(std::ostream& os, const nanos& v){
        unsigned long long uns;
        if(v.cnt<0){                                                  
            os << '-';
            uns = -v.cnt;
        }else{                                                         
            uns = v.cnt;
        }                                                              
        // FIXME core123 should have an raii stream-option manager...
        auto fillwas = os.fill('0');
        auto flagswas = os.flags(std::ios::right | std::ios::dec);
        os << uns/billion_ull << "." << std::setw(9) << uns%billion_ull;
        os.flags(flagswas);
        os.fill(fillwas);
        return os;
    }
};

} // namespace core123


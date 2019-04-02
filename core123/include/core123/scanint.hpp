#pragma once

#include "str_view.hpp"
#include <limits>
#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <cstring> // memcpy, memcmp
#include <string>

// Another 'useful' little function.
//
// template <typename Itype, int base=0, bool skip_white=false>
// size_t
// scanint(str_view sv, Itype* valuep, size_t start=0)
//
//   If an integer representation of a value of Itype is at the start
//   offset in sv, then return a pair consisting ofthe value and the
//   offset of the character following the representation.  Otherwise,
//   throw a std::invalid_argument.
//
//   If skip_white is true, whitespace characters at sv[start] are
//   skipped before conversion is attempted.
// 
//   If Itype is signed, the post-whitespace input may start with an
//   optional '-' sign which indicates that a negative value should be
//   obtained by negating the result of the unsigned conversion of the
//   digits following the sign character (with care taken to detect
//   overflows).
//
//   The integer is expected to be represented in the given base.
//   Digits are 0-9a-z (lower-case only!).  If the base is 0, then the
//   first few characters (after optional whitespace and sign) to be
//   converted determine the base.  A leading "0x" is equivalent to
//   calling scanint with base=16.  A leading '0', not followed by an
//   'x' is equivalent to calling scanint with base=8.  Any other
//   leading characters are equivalent to calling scanint with
//   base=10.
//
//   When base==16 (either explicitly, or by inference with base==0)
//   then the input may start with 0x, which is ignored if it is
//   followed by additional hex digits.  If the initial 0x is not
//   followed by additional hex digits, the returned value is 0
//   and the offset points to the 'x'.
//
//   All this legalese implies:
//
//   base=any  "0"    -> {0, 1}
//   base<=16  "0goo" -> {0, 1}
//   base<34   "0x"   -> {0, 1}
//   base<34   "0xgoo"-> {0, 1}
// 
//   For example:
//      const char *p = ???;
//      int i;
//      scanint<int>(p, &i); // simplest possible use-case
//    
//      uint64_t u;
//      next = scanint(p, &u);  
//      if(next != p.size())
//          throw "oops - we were expecting a number followed by end-of-string";
//
// The decision to forbid '-' on unsigned conversions, and to forbid
// the '+' sign are all inspired by std::from_chars in C++17.  On the
// other hand, the decisions to use string_view, accept upper-case
// letters as digits when base>10, skip leading whitespace, support
// base=0, permit 0x when base=16 and to throw on error all differ
// from std::from_chars.  One day, libstdc++ will implement
// std::from_chars and we'll be able to figure out which we like
// better...

static_assert( '1' - '0' == 1 &&
               '2' - '0' == 2 &&
               '3' - '0' == 3 &&
               '4' - '0' == 4 &&
               '5' - '0' == 5 &&
               '6' - '0' == 6 &&
               '7' - '0' == 7 &&
               '8' - '0' == 8 &&
               '9' - '0' == 9 &&
               'z' - 'a' == 25 &&
               'a' > '9' &&
               'Z' - 'A' == 25 &&
               'A' > '9' &&
               'a' - 'A' == 0x20, // so _casecmp "works"
               "scanint:  We're lost if digits and letters aren't contiguous in the character set with letters above numbers and lowercase above uppercase by exactly 32 ..."
               );

namespace core123{
// _digitcvt<base>(char) - convert a single character to an integer in
//  the range [0, ..., base-1].  Return base if the character does not
//  represent a digit in the specified base.  Does not throw.
template <int base>
int _digitcvt(char c){
    static_assert(base>=2 && base<=36, "digit conversion only works for base in [2, 36]");
    int delta = int(c) - int('0');
    if(delta < base && delta >= 0 && delta < 10)
        return delta;
    if(base <= 10)
        return base;
    delta = int(c) - int('a');
    if(delta+10 < base && delta >= 0)
        return delta+10;
    delta = int(c) - int('A');
    if(delta+10 < base && delta >= 0)
        return delta+10;
    return base;
}

inline bool _casecmp(char letter, char lowercase){
    return (letter|0x20) == lowercase;
}

inline bool _casecmp4(const char *letters, const char *lowercase){
    static_assert(sizeof(char)==1 && sizeof(uint32_t)==4, "we're lost with crazy sizes");
    uint32_t ua;
    ::memcpy(&ua, letters, 4);
    ua |= 0x20202020;
    return ::memcmp(&ua, lowercase, 4) == 0;
}

inline size_t _skip_white(str_view sv, size_t start){
    auto p0 = sv.data();
    auto p = p0;
    auto *end = p0 + sv.size();
    p += start;
#if 0
    while(p<end && std::isspace(*p))
        p++;
#else
    // significantly (i.e., O(1) ns/byte) faster with gcc6 on x86_64.
    while(p<end)
        switch(*p){
        case ' ':
        case '\n':
        case '\t':
        case '\f':
        case '\r':
            p++;
            continue;
        default:
            return p-p0;
        }
#endif
    return p-p0;
}

template <typename Utype, int base>
typename std::enable_if<std::is_unsigned<Utype>::value, size_t>::type
_scanuint(str_view sv, Utype* valuep, size_t start=0){
    static_assert(base>0, "_scanuint does not auto-detect the base");
    constexpr Utype umax = std::numeric_limits<Utype>::max();
    constexpr Utype umax10 = umax/base;
    constexpr unsigned umax10rem = umax%base;
    Utype ret = 0;
        
    char const *p = sv.data();
    char const* end = p + sv.size();
    p += start;
    char const *p0 = p;
    while(p<end){
        unsigned digit = _digitcvt<base>(*p);
        if(digit == base)
            break;
        ++p;
        if( ret > umax10 || (ret == umax10 && digit > umax10rem) )
            throw std::invalid_argument("scanint: overflow: result_so_far=" + std::to_string(ret) + " nextdigit=" + std::to_string(digit));
        ret *= base;
        ret += digit;
    }
    if(p==p0)
        throw std::invalid_argument("_scanuint: no digits");
    *valuep = ret;
    return start+(p-p0);
}

[[noreturn]] inline void
_scanint_rethrow(const char *func, str_view sv, size_t start){
    std::string s(func);
    if(sv.size() < start){
        s += ": error occured with start=" + std::to_string(start) + " > sv.size()=" + std::to_string(sv.size()) + ":  sv[0...]: ";
        start = 0;
    }else{
        s += ": error occured at pos=" + std::to_string(start) + " sv[pos...]: ";
    }
    size_t l = sv.size() - start;
    if(l > 32)
        s += std::string(sv.substr(start, 32)) + "...";
    else
        s += std::string(sv.substr(start, l));
    std::throw_with_nested(std::invalid_argument(s));
}

template <typename Utype, int base=0, bool skip_white=true>
typename std::enable_if<std::is_unsigned<Utype>::value && !std::is_same<Utype, bool>::value, size_t>::type
scanint(str_view sv, Utype* valuep, size_t start=0) try {
    char const *p = sv.data();
    if(skip_white)
        start = _skip_white(sv, start);
    auto sz = sv.size();
    if(base==0){
        if(start >= sz)
            throw std::invalid_argument("scanint<base=0>(start=" + std::to_string(start) + ", sv.size=" + std::to_string(sv.size()) + ")");
        if(p[start] == '0'){
            if(start+1 == sz){
                *valuep = 0;
                return start+1;
            }
            if(p[start+1] == 'x' || p[start+1] == 'X'){
                if(start+2==sz || _digitcvt<16>(p[start+2])==16){
                    *valuep = 0; // 0xgoo
                    return start+1;
                }else
                    return _scanuint<Utype, 16>(sv, valuep, start+2);
            }else{
                if(_digitcvt<8>(p[start+1]) == 8){
                    *valuep = 0;  // 09abc
                    return start+1;
                }else
                    return _scanuint<Utype, 8>(sv, valuep, start+1);
            }
        }
        return _scanuint<Utype, 10>(sv, valuep, start);
    }
    // if base==16, skip initial 0x
    if(base==16 && start+2 < sz && p[start]=='0' &&
       (p[start+1]=='x' || p[start+1]=='X') &&
       _digitcvt<16>(p[start+2]) != 16)
        start += 2;
    // this is unreachable when base==0, but the compiler still
    // has to instantiate it, and static assertions forbid
    // instantiating _scanuint<Utype, 0>, so:
    constexpr auto basenz = base?base:2;
    return _scanuint<Utype, basenz>(sv, valuep, start);
 }catch(std::exception&){
    _scanint_rethrow(__func__, sv, start);
 }

template <typename Itype, int base=0, bool skip_white=true>
typename std::enable_if<std::is_signed<Itype>::value, size_t>::type
scanint(str_view sv, Itype* valuep, size_t start=0) try {
    // I'm not going to think about this until there's an actual
    // example of this static_assert blowing...
    static_assert( -std::numeric_limits<Itype>::max() - 1 == std::numeric_limits<Itype>::min(),
                   "scanint: We're lost if the type doesn't look twos-complement");
    char const* p = sv.data();
    if(skip_white)
        start = _skip_white(sv, start);
    using Utype = typename std::make_unsigned<Itype>::type;
    auto sz = sv.size();
    if(start >= sz)
        throw std::invalid_argument("scanint: no digits");
    bool neg = (p[start] == '-');
    Utype u;
    size_t r = scanint<Utype, base, false>(sv, &u, start+neg);
    if(u == 0){
        *valuep = 0; // handle "-0" before doing r.first -= neg.
        return r;
    }
    u -= neg;
    if(u > Utype(std::numeric_limits<Itype>::max()))
        throw std::invalid_argument("scanint: " + std::to_string(u+neg) + " exceeds signed maximum");
    *valuep = neg ? (-Itype(u) - 1) : u;
    return r;
}catch(std::exception&){
    _scanint_rethrow(__func__, sv, start);
 }

// bool conversion is, mostly, analogous to:
//    is >> b
// but we also accept mixed-case spellings of "true" and "false"
// (e.g., "False", "tRUe", etc) even if base is 16 or more.
// The rules for istreams can be surprising: Any digit-sequence that
// evaluates to a number other than 0 or 1, e.g., "2" or "010" is a
// conversion failure.  There are other surprises, like: "falsexyz"
// converts to false, but "0xyz" (with base=0 or base=16) throws.
//
// Advice: avoid surprises by restricting the input to: "true",
// "false", "0" or "1", followed either by a non-digit-non-Xx or
// end-of-string.
template <typename Btype, int base=0, bool skip_white=true>
typename std::enable_if<std::is_same<Btype, bool>::value, size_t>::type
scanint(str_view sv, Btype* valuep, size_t start=0) try{
    if(skip_white)
        start = _skip_white(sv, start);

    if(sv.size() >= start+4 &&
       // ::strncasecmp(&sv[start], "true", 4) == 0
       // _casecmp(sv[start], 't') && _casecmp(sv[start+1], 'r') && _casecmp(sv[start+2], 'u') && _casecmp(sv[start+3], 'e')
       _casecmp4(&sv[start], "true")
       ){
    *valuep = true;
        return 4+start;
    }
    if(sv.size() >= start+5 &&
       // ::strncasecmp(&sv[start], "false", 5) == 0
       // _casecmp(sv[start], 'f') && _casecmp(sv[start+1], 'a') && _casecmp(sv[start+2], 'l') && _casecmp(sv[start+3], 's') && _casecmp(sv[start+4], 'e')
       _casecmp4(&sv[start], "fals") && _casecmp(sv[start+4], 'e')
       ){
        *valuep = false;
        return 5+start;
    }
    unsigned short us;
    auto ret = scanint<unsigned short, base, false>(sv, &us, start);
    if(us>1)
        throw std::invalid_argument("scanint<bool,...>: unsigned short conversion must be 0 or 1");
    *valuep = us;
    return ret;
 }catch(std::exception&){
    _scanint_rethrow(__func__, sv, start);
 }
}// namespace core123

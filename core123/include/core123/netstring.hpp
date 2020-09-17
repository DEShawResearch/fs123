#pragma once
#include <string>
#include <stdexcept>
#include <iostream>
#include <core123/scanint.hpp>
#include <core123/svto.hpp>

// Some functions that work with "netstrings" that look like:
//
//     <len>:<exactly len bytes of data>,
//
// where <len> is written in base-10 decimal.
//
// See:
//   https://cr.yp.to/proto/netstrings.txt
//   https://tools.ietf.org/html/draft-bernstein-netstrings-02
//
// Note that the ietf draft is stricter, explicitly forbidding leading
// zeros on the length.  '0' may be the first character of the length
// only if it is the only character in the length.  The code here
// follows the stricter rule.
namespace core123 {

// netstring(str_view) - "format" a str_view into a std::string as a
// netstring.
inline std::string netstring(core123::str_view sv){
    auto ret = std::to_string(sv.size());
    ret.reserve(ret.size() + sv.size() + 2);
    ret.append(1, ':');
    ret.append(sv.data(), sv.size());
    ret.append(1, ',');
    return ret;
}

// template <bool skip_white=true>
// size_t svscan_netstring(str_view sv, str_view* svp, size_t start)
//
// If the template parameter skip_white is true, then skip over any
// whitespace at sv[start].  If what follows is a strictly conforming
// netstring, then set svp to point to netstring's payload and return
// the offset of the character following the netstring.  Otherwise,
// throw an exception.  If an exception is thrown, *svp is unchanged.
template <bool skip_white=true>
inline size_t svscan_netstring(core123::str_view sv, core123::str_view* svp, size_t start){
    size_t len;
    if(skip_white)
        start = svscan(sv, nullptr, start);
    // The leading-zero check feels excessive.
    auto colon = scanint<size_t, 10, false>(sv, &len, start);
    if(sv.at(start)=='0'){
        if(len!=0 || colon-start != 1)
            throw std::runtime_error("svscan_netstring:  leading zeros");
    }
        
    // rely on sv.at() to do bounds-checking.
    if(sv.at(colon) != ':')
        throw std::runtime_error("svscan_netstring: no colon");
    if(sv.at(colon+len+1) != ',')
        throw std::runtime_error("svscan_netstring: no trailing commma");
    // No max_size because we're not being asked to allocate space.
    // We're just being asked to point into a pre-existing str_view.
    *svp = sv.substr(colon+1, len);
    return colon+len+2;
}

// sput_netstring - write the contents of sv to an ostream, formatted
// as a netstring.  An exception is thrown only in the unlikely event
// that std::to_string(size_t) throws.  Otherwise, errors are reported
// in the usual way via the output stream's state.
inline std::ostream& sput_netstring(std::ostream& out, core123::str_view sv) {
    // use to_string to format the size, sidestepping out's state.
    // According to C++11, to_string formats as if by sprintf("%u")
    // (with an appropriate width modifier), and AFAIK, %u formating
    // is not affected by the LOCALE.  So we should be good...
    out << std::to_string(sv.size()) << ':';
    out.write(sv.data(), sv.size());
    out << ',';
    return out;
}

// sget_netstring(istream& inp, string* sp, size_t max_size=999999999u)
//
// Skip leading whitespace in the istream, inp.  If inp is at EOF,
// return false.  Otherwise, try to parse a netstring from inp.  If
// the inp contains a netstring with length no more than max_size,
// read the netstring's payload in *sp.  Upon success, return true,
// leaving inp's read pointer immediately following the netstring.
//
// If any errors are detected (i/o errors, premature eof, input that
// doesn't strictly conform to the netstring spec, or a netstring
// longer than max_size), an exception is thrown.  If an exception is
// thrown, inp's failbit is set, its read pointer is undefined and a
// default-constructed empty string is swapped into *sp, discarding
// *sp's previous contents as well as any memory that may have been
// reserved in *sp for the netstring payload.
//
// N.B. the default value of max_size comes from the example code
// in the specs, which uses scanf("%9u") to read the length.
inline bool sget_netstring(std::istream& inp, std::string* sp, size_t max_size = 999999999u) try {
    inp >> std::ws;
    if(inp.eof())
        return false;
    int c;
    size_t sz = 0;
    // scanf(%u) and istream >> integral are far too permissive.  Do
    // the conversion ourselves...
    bool first_time = true;
    while (1) {
        c = inp.get();
        if (c == ':')
            break;
        if(!inp.good()) // i.e., any of fail, bad or eof
            throw std::runtime_error("sget_netstring:  reading from inp failed before end of digits");
        if(!first_time && sz==0)
            throw std::runtime_error("sget_netstring:  leading zeros not allowed on length");
        first_time = false;
        size_t digit = c - '0';
        // N.B.  c-'0' is signed, and might be negative, but in that
        // case, when converted to size_t, it will be (much) larger
        // than 10.
        if(digit >= 10)
            throw std::runtime_error("sget_netstring: expected a digit or colon");
        size_t oldsz = sz;
        sz = sz * 10 + digit;
        if(oldsz > sz)
            throw std::runtime_error("sget_netstring: length doesn't fit in a size_t");
    }
    if(sz > max_size)
        throw std::runtime_error("sget_netstring: length exceeds max_size argument");
    // N.B. sz might be *very* large.  resize might throw bad_alloc, or, worse,
    // it might succeed, and then inp.read might fail, leaving us with a huge
    // string containing garbage.  We therefore clear *sp in the catch block.
    sp->resize(sz);
    inp.read(&(*sp)[0], sz);
    if(!inp.good())  // i.e., any of fail, bad or eof
        throw std::runtime_error("sget_netstring: failed to read " + std::to_string(sz) + " bytes");
    c = inp.get();
    if (c != ',')
        throw std::runtime_error("sget_netstring: did not get terminal comma");
    return true;
 }catch(std::exception&){
    // If there's trouble, set inp's failbit and clear *svp.
    inp.setstate(std::ios::failbit);
    std::string().swap(*sp);
    throw;
 }

} // namespace core123

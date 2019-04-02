#pragma once

#include <core123/scanint.hpp>

// Fs123 custom HTTP headers all start with fs123-

#define HHCOOKIE    "fs123-estalecookie"
#define HHERRNO	    "fs123-errno"
#define HHNO	    "fs123-nextoffset"
#define HHTRSUM	    "fs123-trsum"

static const int fs123_protocol_major = 7;
static const int fs123_protocol_minor_min = 0;
static const int fs123_protocol_minor_max = 2;
static const int fs123_protocol_minor_default = 2;

// parse_quoted_etag is used on server-side and client-side.  This
// seems like as good a place as any...

// parse_quoted_etag: parse a quoted ETag into a uint64.  Throws an
// invalid_argument if it doesn't parse cleanly into a uint64, or if
// it parses in a way that might be ambiguous (e.g., "0123" is a
// numerical match, but not a char-by-char match to "123").
inline
uint64_t parse_quoted_etag(core123::str_view et_sv) {
    // Ignore anything preceding the first ".  This incorrectly
    // permits bogus contents like: abcd"1234", but so what...
    auto qidx = et_sv.find('"');
    if(qidx >= et_sv.size()) // no " or ends with "
        throw std::invalid_argument("parse_quoted_etag:  no double-quote");
    if(et_sv[qidx+1] == '0') // ambiguous.  
        throw std::invalid_argument("parse_quoted_etag:  ambiguous leading 0");
    uint64_t et64;
    // rfc7232 says ETag can't contain whitespace.  If it does,
    // scanint<..,..,false> will throw.
    auto qidx2 = core123::scanint<uint64_t, 10, false>(et_sv, &et64, qidx+1);
    if(qidx2 >= et_sv.size() || et_sv[qidx2] != '"')
        throw std::invalid_argument("parse_quoted_etag:  no trailing double-quote");
    return et64;
 }

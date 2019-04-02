#include "backend123.hpp"
#include "fs123/httpheaders.hpp"
#include <core123/complaints.hpp>
#include <core123/strutils.hpp>
#include <core123/throwutils.hpp>
#include <string>

using namespace core123;

std::atomic<unsigned long> req123::cachetag;
std::atomic<int> req123::default_stale_if_error;
std::atomic<int> req123::default_past_stale_while_revalidate;

// to link with unit tests, this seems to be the best place for proto_minor
int proto_minor;

namespace{

std::string
add_cachetag(std::string& url, bool hasquery) {
    if (req123::cachetag) {
	if (hasquery)
	    url += ";";
	else
	    url += "?";
	url += std::to_string(req123::cachetag);
    }
    return url;
}
}

std::string 
backend123::add_sigil_version(const std::string& urlpfx) /*static*/ {
    if(endswith(urlpfx, "/"))
        complain(LOG_WARNING, "urlpfx ends with /.  This is allowed, but it might be confusing");
    if(urlpfx.find("/fs123/")!=std::string::npos || endswith(urlpfx, "/fs123"))
        throw se(EINVAL, fmt("urlpfx (%s) may not contain a path component \"/fs123\"", urlpfx.c_str()));

    // special case for backward-compatibility... The 7.0 servers don't
    // grok /7/0 any better than they grok /7/1 !!
    // N.B.  Don't carry this over to the /8/ protocol.  If and when
    // we move to /8/, everyone should expect /8/0
    if(proto_minor==0)
        return urlpfx + "/fs123/" + std::to_string(fs123_protocol_major);
    return urlpfx + "/fs123/" + std::to_string(fs123_protocol_major) + '/' + std::to_string(proto_minor);
}

req123
req123::attrreq(const std::string& name, int max_stale) /*static*/ {
    std::string escname = urlescape(name);
    std::string ret = "/a" + escname;
    return {add_cachetag(ret, false), max_stale};
}    

req123
req123::dirreq(const std::string& name, uint64_t ckib, bool begin, int64_t chunkstart) /*static*/ {
    std::string escname = urlescape(name);
    std::string ret = "/d" + escname + "?" + std::to_string(ckib) + ";" +
	std::to_string(begin) + ";" + std::to_string(chunkstart);
    return {add_cachetag(ret, true), MAX_STALE_UNSPECIFIED};
}    

req123
req123::filereq(const std::string& name, uint64_t ckib, int64_t chunkstartkib, int max_stale) /*static*/ {
    std::string escname = urlescape(name);
    std::string ret = "/f" + escname + "?" + std::to_string(ckib) + ";" + std::to_string(chunkstartkib);
    return {add_cachetag(ret, true), max_stale};
}

req123
req123::linkreq(const std::string& name) /*static*/ {
    std::string escname = urlescape(name);
    std::string ret = "/l" + escname;
    return {add_cachetag(ret, false), MAX_STALE_UNSPECIFIED};
}

req123
req123::statfsreq(const std::string& name) /*static*/ {
    std::string escname = urlescape(name);
    std::string ret = "/s" + escname;
    return {add_cachetag(ret, false), MAX_STALE_UNSPECIFIED};
}

req123
req123::statsreq() /*static*/ {
    std::string ret = "/n";
    return {add_cachetag(ret, false), MAX_STALE_UNSPECIFIED};
}

req123
req123::xattrreq(const std::string& name, uint64_t ckib,
			  const char *attrname) {
    std::string escname = urlescape(name);
    std::string ret = "/x" + escname + "?" + std::to_string(ckib) + ";";
    if (attrname) {
	ret += urlescape(attrname);
    }
    ret += ";";
    return {add_cachetag(ret, true), MAX_STALE_UNSPECIFIED};
}


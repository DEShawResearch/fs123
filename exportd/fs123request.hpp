#pragma once
#include <core123/autoclosers.hpp>
#include <core123/strutils.hpp>
#include <system_error>
#include <string>
#include <memory>
#include <event2/http.h>
#include <event2/buffer.h>

// The fs123Req constructor is a convenient way to decompose an fs123
// request into its component parts.  It's constructable from an
// evhttp_request, and the pointers in it have the same lifetime as
// the evhttp_request it was constructed from.  The fs123Req itself is
// a convenient container to pass on to a server-specific backend.
//
// The fs123Req constructor may throw.  In that case, the caller
// should probably abandon the request, e.g., by calling
// evhttp_send_reply or evhttp_send_error.
//
// The fs123Reply class is a convenient container for a
// server-specific backend request-handler to return.  It's members
// correspond to the arguments of evhttp_send_reply.  Use the
// add_out_header() member functions to add to the output headers of
// its associated evhttp_request.  Use the send() method to call
// evhttp_send_reply.
//
// Suggested usage (in an evhttp callback):
//     cb(evhttp_request* evreq, void* arg){
//      try{
//        fs123Req req123{evreq};
//        fs123Reply reply = myfunction(req123, arg);
//        reply.send();
//      }catch(std::exception& e){
//        evhttp_send_error(evreq, 503, e.what());
//      }
//     }
//
// Warning: The evhttp API of libevent is NOT thread-safe.  This means
// that fs123Reply::send (or, if you prefer to call them directly,
// evhttp_reply_send and evhttp_reply_error) *must* be called in the
// same thread as the main event loop.  But see fs123p7exportd_threadpool.cpp
// for a way decode the request (i.e., construct an fs123Req) and form the
// reply (i.e., call myfunction) in a separate thread and then deliver the
// fs123Reply object to an event handler activated in the main loop
// which which will ultimately call evhttp_send_{reply,error}.

struct fs123Req {
    // Protocol history:
    //  1 - X-Fs123-Attributes and X-Fs123-Cache-Flush-Trigger
    //  2 - added X-Fs123-Chunk, stopped supporting Ranges
    //  3 - added  tv_nsec to st_mtim, st_atim and st_mtim in X-Fs123-Attrs
    //  4 - Requests MUST have a semicolon-delimited query-string containing:  
    //         c=<chunklen>@chunkstart
    //      Other expected (but unchecked) query-string parameters are:
    //         t=<trigger>;p=<protocol>
    //      Replies no longer contain a Vary header.
    //      These request headers are ignored:
    //        X-Fs123-Cache-Flush-Trigger, X-Fs123-Protocol, X-Fs123-Chunk
    //  5 - Range is ignored.
    //      c=0@<chunkstart> is understood to
    //      be a getattr request and does not try to open the file or
    //      directory.
    //  5.1 - added X-Fs123-Content-ThreeRoe - the hexdigest of the ThreeRoe
    //        hash of the content.
    //        added X-Fs123-ESTALE-Cookie - See docs/Fs123Consistency for details.
    //
    //      Note that these header fields added in 5.1 do not require
    //      a protocol change.  Old clients simply ignore them.  New
    //      clients should use them.
    //  6 - don't use or set X-Fs123-CacheFlushTrigger
    //  7 - major, incompatible cleanup/redesign: stop trying to overload
    //      everything into a single request, take advantage of /FUNCTION/
    //      obsoleted /r/ (which did everything and the c=0@0, c=foo@begin)
    //	    cgi-style syntax in favor of /a/ for stat(), /f/ for file
    //	    read, /d/ for directory read, /l/ for readlink, /s/ for statvfs,
    //	    and a new /x/ for listxattr/getxattr. X-Fs123-Attrs is gone,
    //	    replaced by /a/ for the attrs (in the body) and
    //	    new HHERRNO header for the errno. query syntax is now arg;arg;...
    //	    where the args syntax semantics are defined per FUNCTION.
    //	    removed X-Fs123-Protocol header and renamed others to shorter names
    //	    ("X-Fs123-Chunk-Next-Offset" becomes "fs123-nextoffset", referred to in code as HHNO,
    //	    "X-Fs123-Content-ThreeRoe" becomes "fs123-trsum" aka HHTRSUM,
    //	    "X-Fs123-ESTALE-Cookie" becomes "fs123-estalecookie" aka HHCOOKIE,
    //	    which is now sent with the new "fs123-errno" aka HHERRNO)
    //	    

    // The full URL looks like:
    //   http://remote[:port]/SEL/EC/TOR/SIGIL/PROTO/FUNCTION/PA/TH?QUERY
    // A concrete instance might be:
    //    http://fs123.deshawresearch.com:8080/selec/tor/fs123/6/f/path/relative/to/exportroot?128;0;99
    // The /SEL/EC/TOR consists of zero or more /path components.  No
    //    individual path-component may match the /SIGIL.
    // The purpose is to allow http routing rules on front-end systems
    // that like between the fs123 client and backend server.  The
    // fs123 backend ignores selectors at the moment, but the backend
    // may one-day use them to manage multiple exported filesystems with
    // different caching/timeout parameters.
    // The /SIGIL is the literal string "/fs123", it separates
    // the selector from the rest of the URL.
    // The /PROTO is the unsigned decimal integer fs123 protocol number
    // The /FUNCTION is a single path component.  
    //     With protocol v7, these can be one of "/[afdlsx]".  Others may be added in the future.
    // The /PA/TH consists of 0 or more /path components and
    // represents that actual file path (under  the exported filesystem root)
    // for the operation.
    // THe QUERY consists of arg1;arg2;..., see the specification of each FUNCTION
    // for details.
    evhttp_request *evreq_;       
    evhttp_cmd_type method_;   // e.g., EVHTTP_REQ_GET
    const char *path_;         // e.g., /selec/tor/fs123/6/r/path/relative/to/exportroot
    // ??FIXME - would string_view be better than char* ??
    // we wouldn't need function_s_, for example.
    const char * selector_;    // e.g., /selec/tor
    int url_proto_major_;            // /fs123/7/1 -> 7
    int url_proto_minor_;            // /fs123/7/1 -> 1
    std::string path_info_;   // e.g., /path/relative/to/exportroot
    std::string query_;       // e.g., ?c=131072@0;ct=xxx
    std::string function_;    // e.g., r 
    std::string accept_encoding_; // e.g., "fs123-secretbox", or ""
    fs123Req(evhttp_request* evreq);
private:
    std::unique_ptr<char, decltype(std::free)*> decoded_path_;
};

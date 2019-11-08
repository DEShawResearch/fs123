#pragma once

#include <core123/strutils.hpp>
#include <atomic>
#include <string>
#include <vector>
#include <cstdarg>
#include <syslog.h>

// How to complain?  Use one of:
// 
//   complain(string)
//   complain(priority, string)
//   complain(exception, string)
//   complain(priority, exception, string)
//
// The printf-style complaints add a lot of extra
// hair.  They're currently turned off by default
// but can be turned on with -DCOMPLAIN_PRINTF.
// Otherwise, consider using fmt() or str() in
// strutils.hpp.
//
//   complain(fmt, ...)
//   complain(priority, fmt, ...)
//   complain(exception, fmt, ...)
//   complain(priority, exception, fmt, ...)
//   vcomplain(fmt, va_list)
//   vcomplain(priority, fmt, va_list)
//   vcomplain(exception, fmt, va_list)
//   vcomplain(priority, exception, fmt, va_list)
//
//  There is also a wildly incomplete set of additional convenience
//  functions:
//
//    log_notice(string)
//    log_notice(fmt, ...)
//
// All complaints (subject to throttling and level-limiting (see
// below)) are forwarded to a log_channel.  If the DiagKey
// "complaints" is set, then all complaints (subject to level-limiting
// only, not throttling) are send to the diag stream.
//
// All functions are thread-safe.
//
// The 'priority' argument is passed, unchanged, to log channel. As in
// syslog, log channel priorities are the bitwise-or of a optional
// "facility" and a "severity level".  E.g., LOG_ERR, or
// LOG_WARNING|LOG_LOCAL2.
// 
// When an exception argument is present, the exception is recursively
// "un-nested", and each line of each nested exception's .what()
// method is logged separately.
//
// A global sequence number, seq, is maintained for all complaints
// that satisfy the level limit (see below).  For multi-part
// complaints (i.e., nested exceptions), a sub-sequence number, subseq
// is also maintained.  All messages are prepended with:
//
//     L[seqno.subseq]
//
// So grouping and ordering can be reconstructed even if multiple
// threads interleave calls to complain.  L is a one-character indicator
// of the level: emer'G', 'A'lert, 'C'rit, 'E'rr, 'W'arning, 'N'otice,
// 'I'nfo or 'D'ebug.
//
// How to control the volume?
//
// There are two independent "volume contols":
//
// 1 - Level limiting:
//
//     set_complaint_level(int level)
//
// is roughly equivalent setlogmask(LOG_UPTO(level)), but it acts
// before any syslogging, formatting or exception-unnesting takes
// place.  So it's not a huge performance hit to have lots of:
//     complain(LOG_DEBUG, ...)
// as long as they're usually turned off by something like:
//     set_complaint_level(LOG_NOTICE);
//
// The default level is 0, which delivers all messages to the channel.
//
// 2 - Rate throttling:
//
//    The rate of message delivered to the channel can be limited with:
//
//       set_complaint_max_hourly_rate(float)
//
//    If the time-averaged rate of offered messages exceeds the
//    maximum hourly rate, messages will be randomly discarded, so
//    that the delivered rate is approximately the maximum hourly
//    rate.  Discarded messages consume a sequence number.  So it's
//    possible to identify discarded messages after-the-fact by
//    looking for gaps in the logged sequence numbers.  Multi-part
//    message are discarded all-or-none.  Setting the maximum
//    hourly rate to zero or a negative value effectively discards
//    all messages.
//
//    The time-averaged rate is measured over an exponential window
//    that defaults to 1 hour, but that can be changed with:
//
//       set_complaint_averaging_window(float) // in seconds
//
//    Note that the rate limit is still applied to the the average
//    *hourly* rate, even if a different window is used.  It is an
//    error to set the complaint_averaging_window to a negative value.
//
//    The current values of the complaint_level, max_hourly_rate and
//    averaging_window may be retrieved with get_XXX functions.
//
// TODO:
//   - DO NOT turn this into yet another logging library.  If there's
//     significantly more "heavy lifting" to do, then find a way to
//     let somebody else do it (e.g., glog)!!
//   - even more convenience functions?  warn(...), notice(...) info(...) ?
//     It feels wrong to say complain(LOG_INFO, "blah blah blah").
//   - multiple output streams?  (Sounds cool, but see point #1)
//   - a class?  A namespace?  Syslog is naturally a "singleton".  There can be
//     only one.  So there's not much value in class-level encapsulation.  A
//     namespace (and a 'detail' sub-namespace) might be cleaner than having
//     "private members" start with underscore.
//   - header-only?  There's not *that much* code in _do_complaint and
//     _whatnest.  But the statics are trouble.  Furthermore, we sit
//     on top of log_channel, which is not header-only, so there's
//     little to be gained.
//   - POSIX syslog groks %m, but POSIX vsprintf does not.  Glibc's vsprintf
//     does, and we're relying on that.  For maximum portability, we should
//     do it ourselves.
// 

// "private" members:

namespace core123{
// _do_complaint - responsible for rate-throttling and delivery
void _do_complaint(int priority, const std::vector<std::string>& vs);
// _whatnest - responsible for unnesting, sequence-numbering and
//     newline termination.
std::vector<std::string> _whatnest (int priority, const std::string& pfx, const std::exception* ep = nullptr);
std::vector<std::string> _whatnest (int priority, const std::string& pfx, char const* const* b, char const* const* e);
extern std::atomic<int> _complaint_level;

void set_complaint_destination(const std::string&, int mode);
void reopen_complaint_destination();
void set_complaint_max_hourly_rate(float rate);
float get_complaint_max_hourly_rate();
void set_complaint_averaging_window(float averaging_window);
float get_complaint_averaging_window();
inline void set_complaint_level(int newlevel){ _complaint_level = newlevel&0x7; }
inline int get_complaint_level(){ return _complaint_level; }    

// If delta timestamps are on, then the first [N.0] sub-record of
// every complaint gets an additional +%.3f delta timestamp.
// Delta timestamps are seconds since start_complaint_delta_timestamp()
// was called.  start_complaint_delta_timestamp() also calls
// complain(LOG_NOTICE, ...) to add a record like this:
// 
//     N[4.0]+0.000 start_complaint_delta_timestamp: 1524838995.000 2018-04-27 10:23:15-0400
// 
void start_complaint_delta_timestamps();
void stop_complaint_delta_timestamps();    

float get_complaint_hourly_rate();

// First the methods at the "bottom" of the call chain:
// These  call _do_complaint and _whatnest.
inline void complain(int priority, const std::string& msg){
    if((priority&0x7) <= _complaint_level)
        _do_complaint(priority, _whatnest(priority, msg));
}

inline void vcomplain(int priority, const char *fmt, va_list ap){
    if((priority&0x7) <= _complaint_level)
        _do_complaint(priority, _whatnest(priority, core123::vfmt(fmt, ap)));
}    

inline void complain(int priority, const std::exception& e, const std::string& msg){
    if((priority&0x7) <= _complaint_level)
        _do_complaint(priority, _whatnest(priority, msg, &e));
}

// Methods that don't specify priority (assume it's LOG_ERR)
inline void complain(const std::exception& e, const std::string& msg){
    complain(LOG_ERR, e, msg);
}

inline void complain(const std::string& msg){
    complain(LOG_ERR, msg);
}

inline void log_notice(const std::string& msg){
    complain(LOG_NOTICE, msg);
}

// Now for the overloads that that take a printf-style format string
// and a ... arglist.  They can be disabled with a #define.
#if !defined(NO_CORE123_FORMATTED_COMPLAINTS)
inline void vcomplain(int priority, const std::exception &e, const char *fmt, va_list ap) {
    if((priority&0x7) <= _complaint_level)
        _do_complaint(priority, _whatnest(priority, core123::vfmt(fmt, ap), &e));
}

inline void complain(int priority, const char *fmt, ...)
    __attribute__ ((__format__ (__printf__, 2, 3)));
inline void complain(int priority, const char *fmt, ...){
    va_list args;
    va_start(args, fmt);
    vcomplain(priority, fmt, args);
    va_end(args);
}    

inline void complain(int priority, const std::exception& e, const char *fmt, ...)
    __attribute__ ((__format__ (__printf__, 3, 4)));
inline void complain(int priority, const std::exception& e, const char *fmt, ...){
    va_list args;
    va_start(args, fmt);
    vcomplain(priority, e, fmt, args);
    va_end(args);
}

inline void vcomplain(const char *fmt, va_list ap){
    vcomplain(LOG_ERR, fmt, ap);
}

inline void vcomplain(const std::exception &e, const char *fmt, va_list ap) {
    vcomplain(LOG_ERR, e, fmt, ap);
}

inline void complain(const std::exception& e, const char *fmt, ...)
    __attribute__ ((__format__ (__printf__, 2, 3)));
inline void complain(const std::exception& e, const char *fmt, ...){
    va_list args;
    va_start(args, fmt);
    vcomplain(LOG_ERR, e, fmt, args);
    va_end(args);
}

inline void complain(const char *fmt, ...)
    __attribute__ ((__format__ (__printf__, 1, 2)));
inline void complain(const char *fmt, ...){
    va_list args;
    va_start(args, fmt);
    vcomplain(LOG_ERR, fmt, args);
    va_end(args);
}    

inline void log_notice(const char *fmt, ...)
    __attribute__ ((__format__ (__printf__, 1, 2)));
inline void log_notice(const char *fmt, ...){
    va_list args;
    va_start(args, fmt);
    vcomplain(LOG_NOTICE, fmt, args);
    va_end(args);
}
#endif
} // namespace core123

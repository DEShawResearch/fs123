#pragma once

#include <core123/strutils.hpp>
#include <core123/log_channel.hpp>
#include <core123/diag.hpp>
#include <core123/datetimeutils.hpp>
#include <core123/exnest.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <random>
#include <mutex>
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

// complainer_t is a singleton class that bundles state variables and
// methods that were previously in file-scope statics in
// complaints.cpp.  The complaint API has always been via free
// functions in the core123 namespace.  The free functions are now
// friends of the new singleton class.
struct complainer_t{
    friend void set_complaint_destination(const std::string&, int mode);
    friend void reopen_complaint_destination();
    friend void set_complaint_max_hourly_rate(float rate);
    friend float get_complaint_max_hourly_rate();
    friend void set_complaint_averaging_window(float averaging_window);
    friend float get_complaint_averaging_window();
    friend void set_complaint_level(int newlevel);
    friend int get_complaint_level();
    friend void complain(int priority, const std::string& msg);
    friend void vcomplain(int priority, const char *fmt, va_list ap);
    friend void complain(int priority, const std::exception& e, const std::string& msg);
#if !defined(NO_CORE123_FORMATTED_COMPLAINTS)
    friend void vcomplain(int priority, const std::exception &e, const char *fmt, va_list ap);
#endif        

    // If delta timestamps are on, then the first [N.0] sub-record of
    // every complaint gets an additional +%.3f delta timestamp.
    // Delta timestamps are seconds since start_complaint_delta_timestamp()
    // was called.  start_complaint_delta_timestamp() also calls
    // complain(LOG_NOTICE, ...) to add a record like this:
    // 
    //     N[4.0]+0.000 start_complaint_delta_timestamp: 1524838995.000 2018-04-27 10:23:15-0400
    // 
    friend void start_complaint_delta_timestamps();
    friend void stop_complaint_delta_timestamps();    

    friend float get_complaint_hourly_rate();
private:    
    log_channel logchan;
    std::atomic<int> _complaint_level;
    std::atomic<int> seq_atomic;
    
    float averaging_window = 3600.;  // one hour
    float max_hourly_rate = 1.e9;    // unlimited.
    std::chrono::system_clock::time_point tlast = std::chrono::system_clock::now();
    float rate = 0.;
    // No need for fancy seeding of the engine.  We're not doing Monte Carlo here.
    std::default_random_engine rand_eng;
    std::uniform_real_distribution<float> u01; // could use generate_canonical instead
    std::mutex mtx; // protects the throttling vars *and* keeps the logs together
    bool delta_timestamps = 0;
    std::chrono::system_clock::time_point delta_timestamp_zero;

    // complainer_t is a singleton.  Only one will ever be created
    // by the_complainer().
    friend complainer_t& the_complainer();

    // Methods:
    complainer_t() :
        logchan("%stderr", 0),
        _complaint_level{LOG_DEBUG},
        seq_atomic{0}
    {}

    // _do_complaint - responsible for rate-throttling and delivery
    void _do_complaint(int priority, const std::vector<std::string>& vs){
        static auto _complaints = diag_name("complaints");
        std::lock_guard<std::mutex> lgd(mtx);
        // Do not throttle the diag stream.
        if(_complaints){
            for(const auto& s : vs){
                DIAG(_complaints, s);
            }
        }

        auto now = std::chrono::system_clock::now();
        float deltat = core123::dur2dbl(now - tlast);
        tlast = now;
        // The "instantaneous" hourly rate estimate is
        // v.size()/deltat.  But we fold it into an exponential
        // moving average over the averaging_window.  In the limit of
        // large deltat (>>window), alpha is 0 and rate is
        // v.size()/deltat_hours. In the limit of small deltat, alpha is
        // (1-dt/window) and we add v.size()/window_hours to the rate.  The
        // steady-state rate is: v.size()/deltat_hours
        //   
        // N.B.  An accurate exponential isn't necessary here.  The
        // exponential gives the filter some "nice" translation-invariance
        // properties, but it's not really important.  A linear window, or
        // an 'exp-ish' window should be fine.
        float one_minus_alpha = -expm1f(-deltat/averaging_window);
        float deltat_hours = deltat/3600.;
        float alpha = 1. - one_minus_alpha; // exp(-deltat/averaging_window)
        rate *= alpha;
        rate += one_minus_alpha * vs.size()/deltat_hours;
        // Keep levels "above" LOG_ERR (e.g., LOG_CRIT, LOG_ALERT) or if
        // the current rate is below max_rate.  If it's LOG_ERR or below,
        // and the current rate is above max_rate, then keep this bundle
        // of vs.size() messages with probability:
        // max_rate/(vs.size()*rate).
        auto level = priority & 0x7;
        bool keep = level < LOG_ERR || ( rate < max_hourly_rate ) || u01(rand_eng) < max_hourly_rate / (vs.size() * rate);
        // We could write another dozen lines of code to try to report on
        // the discards and the rate ... OR NOT... We're putting a
        // sequence-number on every group of messages. We can always look
        // for gaps in the sequence after the fact...
        if(!keep)
            return;

        // Finally - we  can actually deliver our complaints!

        for(const auto& s : vs){
            logchan.send(level, s);
        }
    }

    // _whatnest - responsible for unnesting, sequence-numbering and
    //     newline termination.
    std::vector<std::string> _whatnest (int priority, const std::string& pfx, const std::exception* ep = nullptr){
        using namespace std::chrono;
        // syslogs from multiple threads get interleaved in confusing ways.
        // Attach a sequence number to everything we send to syslog so that
        // we can tease them apart later.
        // level keys:  emerG, Alert, Crit, Err, Warning, Notice, Info, Debug
        char levkey = "GACEWNID"[priority&0x7];
        int seq = seq_atomic++;
        std::vector<std::string> ret;
        if(delta_timestamps){
            ret.push_back(core123::fmt("%c[%d.0]+%.3f %s",
                                       levkey, seq,
                                       duration<double>(system_clock::now() - delta_timestamp_zero).count(),
                                       pfx.c_str()));
        }else{
            ret.push_back(core123::fmt("%c[%d.0] %s", levkey, seq, pfx.c_str()));
        }
        
        if(ep){
            int i=1;
            for(auto& a : exnest(*ep)){
                // If what() has newlines, expand them into separate elements of ret.
                const char *p = a.what();
                const char *e = p + ::strlen(p);
                while(p<e){
                    const char *nl = std::find(p, e, '\n');
                    ret.push_back(core123::fmt("%c[%d.%d] %.*s", levkey, seq, i++, int(nl - p), p));
                    p = nl + (nl < e);
                }
            }
        }
        return ret;
    }

    // FIXME - We have two nearly identical implementations of
    // _whatnest.  That's one too many.  Refactor!
    std::vector<std::string> _whatnest (int priority, const std::string& pfx, char const* const* beg, char const* const* end){
        // syslogs from multiple threads get interleaved in confusing ways.
        // Attach a sequence number to everything we send to syslog so that
        // we can tease them apart later.
        // level keys:  emerG, Alert, Crit, Err, Warning, Notice, Info, Debug
        using namespace std::chrono;
        char levkey = "GACEWNID"[priority&0x7];
        int seq = seq_atomic++;
        std::vector<std::string> ret;
        if(delta_timestamps){
            ret.push_back(core123::fmt("%c[%d.0]+%.3f %s",
                                       levkey, seq,
                                       duration<double>(system_clock::now() - delta_timestamp_zero).count(),
                                       pfx.c_str()));
        }else{
            ret.push_back(core123::fmt("%c[%d.0] %s", levkey, seq, pfx.c_str()));
        }
        
        int i=1;
        for(auto pp = beg; pp<end; ++pp){
            // If what() has newlines, expand them into separate elements of ret.
            const char *p = *pp;
            const char *e = p + ::strlen(p);
            while(p<e){
                const char *nl = std::find(p, e, '\n');
                ret.push_back(core123::fmt("%c[%d.%d] %.*s", levkey, seq, i++, int(nl - p), p));
                p = nl + (nl < e);
            }
        }
        return ret;
    }
};

inline complainer_t& the_complainer(){
    // Code may continue to 'complain', deep into static destructor territory.
    // It may be better to avoid the 'static destructor fiasco' by leaking
    // a new-ed complainer_t that we never delete.
    static complainer_t the;
    return the;
}


inline void set_complaint_destination(const std::string& dest, int mode){
    the_complainer().logchan.open(dest, mode);
}

inline void reopen_complaint_destination(){
    the_complainer().logchan.reopen();
}

inline void set_complaint_max_hourly_rate(float new_rate){
    std::lock_guard<std::mutex> lg(the_complainer().mtx);
    // N.B.  zero or negative is suspicious but not
    // strictly wrong...  we reject everything.
    the_complainer().max_hourly_rate = new_rate;
}

inline float get_complaint_max_hourly_rate(){
    std::lock_guard<std::mutex> lg(the_complainer().mtx);
    return the_complainer().max_hourly_rate;
}

inline void set_complaint_averaging_window(float new_window){
    std::lock_guard<std::mutex> lg(the_complainer().mtx);
    if(new_window < 0.) // 0.0 is ok?
        throw std::runtime_error("set_complaint_averaging_window:  argument must be non-negative");
    the_complainer().averaging_window = new_window;
}

inline float get_complaint_averaging_window(){
    std::lock_guard<std::mutex> lg(the_complainer().mtx);
    return the_complainer().averaging_window;
}
    
inline void set_complaint_level(int newlevel){ the_complainer()._complaint_level = newlevel&0x7; }
inline int get_complaint_level(){ return the_complainer()._complaint_level; }    

// If delta timestamps are on, then the first [N.0] sub-record of
// every complaint gets an additional +%.3f delta timestamp.
// Delta timestamps are seconds since start_complaint_delta_timestamp()
// was called.  start_complaint_delta_timestamp() also calls
// complain(LOG_NOTICE, ...) to add a record like this:
// 
//     N[4.0]+0.000 start_complaint_delta_timestamp: 1524838995.000 2018-04-27 10:23:15-0400
// 
extern void complain(int priority, const std::string& msg);
inline void start_complaint_delta_timestamps(){
    using namespace std::chrono;
    auto& tc = the_complainer();
    {
        std::lock_guard<std::mutex> lgd(tc.mtx);
        tc.delta_timestamps = true;
        tc.delta_timestamp_zero = system_clock::now();
    }
    auto epoch = duration_cast<milliseconds>(tc.delta_timestamp_zero.time_since_epoch()).count();
    int epoch_s = epoch/1000;
    int epoch_mus = epoch%1000;
    char epoch_buf[128];
    auto when = system_clock::to_time_t(tc.delta_timestamp_zero);
    struct tm tm;
    if(::localtime_r( &when, &tm ) == nullptr )
        throw std::runtime_error("start_complaint_delta_timestamps: ::localtime_r failed");
    if(0 == std::strftime(epoch_buf, sizeof(epoch_buf), "%F %T%z", &tm))
        throw std::runtime_error("start_complaint_delta_timestamps:  strftime failed");
    complain(LOG_NOTICE, fmt("complaint_delta_timestamp start time: %d.%06d %s",
                             epoch_s, epoch_mus, epoch_buf));
}

inline void stop_complaint_delta_timestamps(){
    the_complainer().delta_timestamps = false;
}

inline float get_complaint_hourly_rate(){
    std::lock_guard<std::mutex> lg(the_complainer().mtx);
    return the_complainer().rate;
}

// First the methods at the "bottom" of the call chain:
// These  call _do_complaint and _whatnest.
inline void complain(int priority, const std::string& msg){
    auto& tc = the_complainer();
    if((priority&0x7) <= tc._complaint_level)
        tc._do_complaint(priority, tc._whatnest(priority, msg));
}

inline void vcomplain(int priority, const char *fmt, va_list ap){
    auto& tc = the_complainer();
    if((priority&0x7) <= tc._complaint_level)
        tc._do_complaint(priority, tc._whatnest(priority, core123::vfmt(fmt, ap)));
}    

inline void complain(int priority, const std::exception& e, const std::string& msg){
    auto& tc = the_complainer();
    if((priority&0x7) <= tc._complaint_level)
        tc._do_complaint(priority, tc._whatnest(priority, msg, &e));
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
    auto& tc = the_complainer();
    if((priority&0x7) <= tc._complaint_level)
        tc._do_complaint(priority, tc._whatnest(priority, core123::vfmt(fmt, ap), &e));
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

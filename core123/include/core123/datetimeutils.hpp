#pragma once

#include <chrono>
#include <time.h>
#include <string>
#include <core123/strutils.hpp>
#include <core123/throwutils.hpp>

namespace core123 {

// an eclectic selection of "useful" chrono manipulations...

// Enable ins, str and fmt on chrono::duration and chrono::time_point
// by implementing the core123::insertone primitive

// insertone(..., duration) uses nanos (from strutils.hpp).
template<class Rep, class Period>
struct insertone<std::chrono::duration<Rep, Period>>{
    static std::ostream& ins(std::ostream& os, const std::chrono::duration<Rep, Period>& dur){
        return os << nanos(std::chrono::duration_cast<std::chrono::nanoseconds>(dur).count());
    }
};

// insertone(..., time_point tp) calls the duration insertone on tp.time_since_epoch()
template<class Clock, class Duration>
struct insertone<std::chrono::time_point<Clock, Duration>>{
    static std::ostream& ins(std::ostream& os, const std::chrono::time_point<Clock, Duration>& tp){
        return insertone<Duration>::ins(os, tp.time_since_epoch());
    }
};

// A few function to make conversion from chrono to double a little
// less verbose.
template <class Rep, class Period>
double dur2dbl(const std::chrono::duration<Rep, Period>& dur){
    return std::chrono::duration<double>(dur).count();
}

template <typename Clock, typename Duration>
double tp2dbl(const std::chrono::time_point<Clock, Duration>& tp){
    return dur2dbl(tp.time_since_epoch());
}

// Do we want a whole family of these too??
template <typename Clock, typename Duration>
double tpuntildbl(const std::chrono::time_point<Clock, Duration>& tp){
    return dur2dbl(tp - Clock::now());
}

inline std::string timet_to_httpdate(time_t epoch){
    char buf[128]; // more than enough
    // N.B.  %a and %b are locale-dependent.  We should be ok as long
    // as we never call setlocale().
    strftime(buf, sizeof(buf), "%a, %d %b %Y %T GMT", gmtime(&epoch));
    struct tm epoch_tm;
    if(gmtime_r(&epoch, &epoch_tm) == nullptr)
        throw se(EINVAL, "gmtime_r(" + str(epoch) + ")");
    if(0 == strftime(buf, sizeof(buf), "%a, %d %b %Y %T GMT", &epoch_tm))
        throw se(EINVAL, "strftime(buf, \"%a, %d %b %Y %T GMT\", &epoch_tm)");
    return buf;
}

inline time_t httpdate_to_timet(const char *http_date){
    struct tm ttm;
    ttm.tm_isdst = 0;
    // Despite its flaws, I have to wonder whether chrono would be better here...
    auto p = strptime(http_date, "%a, %d %b %Y %T GMT ", &ttm);
    if(*p!='\0') {
        throw std::system_error(EINVAL, std::system_category(),
                                fmt("date_to_timet(%s) failed at character #%zd",
                                    http_date, p - http_date));
    }
    //
#ifndef NO_TIMEGM
    // Linux (and BSD) timegm does the right thing and fast
    return timegm(&ttm);
#else
    // N.B.  mktime works with the *local* time, so subtract 'timezone'
    // from the value returned by mktime.  Could also putenv(TZ), ugh.
    return mktime(&ttm) - timezone;
#endif
}

// traits classes to test whether a template arg is a duration or a time_point:
template <class T>
struct is_duration : std::false_type{};

template<class Rep, class Period>
struct is_duration<std::chrono::duration<Rep, Period>> : std::true_type{};

template <class T>
struct is_time_point : std::false_type{};

template<class Clk, class Dur>
struct is_time_point<std::chrono::time_point<Clk, Dur>> : std::true_type{};

} // namespace core123

// ostream inserter for timespec
inline std::ostream& operator<<(std::ostream& os, const timespec& ts){
    return os << core123::nanos(ts.tv_sec*1000000000ll + ts.tv_nsec);
}

// comparison operators for timespec.
inline bool operator==(const struct timespec& ts1, const struct timespec& ts2){
    return ts1.tv_sec == ts2.tv_sec && ts1.tv_nsec == ts2.tv_nsec;
}

inline bool operator!=(const struct timespec& ts1, const struct timespec& ts2){
    return !(ts1 == ts2);
}

inline bool operator<(const struct timespec& ts1, const struct timespec& ts2){
    return ts1.tv_sec < ts2.tv_sec || (ts1.tv_sec==ts2.tv_sec && ts1.tv_nsec < ts2.tv_nsec) ;
}


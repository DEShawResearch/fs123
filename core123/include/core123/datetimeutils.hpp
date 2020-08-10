#pragma once

#include <chrono>
#include <time.h>
#include <string>
#include <core123/strutils.hpp>
#include <core123/throwutils.hpp>
#include <core123/sew.hpp>

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

template <typename Clock, typename Duration>
Duration until(const std::chrono::time_point<Clock, Duration>& tp){
    return tp - Clock::now();
}

inline std::string timet_to_httpdate(time_t epoch){
    char buf[128]; // more than enough
    struct tm epoch_tm;
    sew::gmtime_r(&epoch, &epoch_tm);
    // N.B.  %a and %b are locale-dependent.  We should be ok as long
    // as we never call setlocale().
    sew::strftime(buf, sizeof(buf), "%a, %d %b %Y %T GMT", &epoch_tm);
    return buf;
}

inline time_t httpdate_to_timet(const char *http_date){
    struct tm ttm;
    ttm.tm_isdst = 0;
    sew::strptime(http_date, "%a, %d %b %Y %T GMT ", &ttm);
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

// time_point_{plus,minus}: The standard rules for adding a time_point
// to a duration are surprising enough that I think 'duration<float>
// considered harmful' is a good rule to follow.  But that rule is not
// widely recognized, and a generic function might be required to add
// or subtract a timepoint and a duration.  Use
// time_point_{plus,minus}.  They return a time_point with the same
// Dur type as the time_point argument.  They differ from the standard
// operator+() and operator-() which return a time_point whose Dur is
// the std::common_type of the Durs of the arguments, which is
// terribly surprising when one is a float and the other is a much
// larger integer, e.g.,
//
//    system_clock::now() + duration<float>(1.0f) // SURPRISE!
//  time_point_plus(system_clock::now(), duration<float>(1.0f)) // closer to what you're expecting
//
template <class Clk, class Dur, class Rep, class Period>
inline std::chrono::time_point<Clk, Dur>
time_point_plus(const std::chrono::time_point<Clk, Dur>& tp, const std::chrono::duration<Rep, Period>& dur){
    return tp + std::chrono::duration_cast<Dur>(dur);
}

template <class Clk, class Dur, class Rep, class Period>
inline std::chrono::time_point<Clk, Dur>
time_point_plus(const std::chrono::duration<Rep, Period>& dur, const std::chrono::time_point<Clk, Dur>& tp){
    return time_point_plus_duration(tp, dur);
}

template <class Clk, class Dur, class Rep, class Period>
inline std::chrono::time_point<Clk, Dur>
time_point_minus(const std::chrono::time_point<Clk, Dur>& tp, const std::chrono::duration<Rep, Period>& dur){
    return tp - std::chrono::duration_cast<Dur>(dur);
}

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


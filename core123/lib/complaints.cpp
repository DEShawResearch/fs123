#include <core123/complaints.hpp>
#include <core123/strutils.hpp>
#include <core123/exnest.hpp>
#include <core123/diag.hpp>
#include <core123/datetimeutils.hpp>
#include <core123/log_channel.hpp>
#include <stdexcept>
#include <algorithm>
#include <random>
#include <chrono>
#include <cmath>
#include <ctime>

using namespace std::chrono;

static auto _complaints = core123::diag_name("complaints");
static float averaging_window = 3600.;  // one hour
static float max_hourly_rate = 1.e9;    // unlimited.
static std::chrono::system_clock::time_point tlast = std::chrono::system_clock::now();
static float rate = 0.;
// No need for fancy seeding of the engine.  We're not doing Monte Carlo here.
static std::default_random_engine e;
static std::uniform_real_distribution<float> u01; // could use generate_canonical instead
static std::mutex mtx; // protects the throttling vars *and* keeps the logs together
static bool delta_timestamps = 0;
static std::chrono::system_clock::time_point delta_timestamp_zero;

namespace core123{
std::atomic<int> _complaint_level{LOG_DEBUG};

log_channel& the_channel(){
    // If complain("foo") is called before set_complaint_destination(...),
    // then "foo" will come out on stderr.  
    static log_channel *p = new log_channel("%stderr", 0);
    return *p;
}

void
set_complaint_destination(const std::string& dest, int mode){
    the_channel().open(dest, mode);
}

void
set_complaint_max_hourly_rate(float new_rate){
    std::lock_guard<std::mutex> lg(mtx);
    // N.B.  zero or negative is suspicious but not
    // strictly wrong...  we reject everything.
    max_hourly_rate = new_rate;
}

float
get_complaint_max_hourly_rate(){
    std::lock_guard<std::mutex> lg(mtx);
    return max_hourly_rate;
}

void
set_complaint_averaging_window(float new_window){
    std::lock_guard<std::mutex> lg(mtx);
    if(new_window < 0.) // 0.0 is ok?
        throw std::runtime_error("set_complaint_averaging_window:  argument must be non-negative");
    averaging_window = new_window;
}

float
get_complaint_averaging_window(){
    std::lock_guard<std::mutex> lg(mtx);
    return averaging_window;
}

float
get_complaint_hourly_rate(){
    std::lock_guard<std::mutex> lg(mtx);
    return rate;
}

void start_complaint_delta_timestamps(){
    {
        std::lock_guard<std::mutex> lgd(mtx);
        delta_timestamps = true;
        delta_timestamp_zero = system_clock::now();
    }
    auto epoch = duration_cast<milliseconds>(delta_timestamp_zero.time_since_epoch()).count();
    int epoch_s = epoch/1000;
    int epoch_mus = epoch%1000;
    char epoch_buf[128];
    auto when = system_clock::to_time_t(delta_timestamp_zero);
    struct tm tm;
    if(::localtime_r( &when, &tm ) == nullptr )
        throw std::runtime_error("start_complaint_delta_timestamps: ::localtime_r failed");
    if(0 == std::strftime(epoch_buf, sizeof(epoch_buf), "%F %T%z", &tm))
        throw std::runtime_error("start_complaint_delta_timestamps:  strftime failed");
    complain(LOG_NOTICE, fmt("complaint_delta_timestamp start time: %d.%06d %s",
                             epoch_s, epoch_mus, epoch_buf));
}

void stop_complaint_delta_timestamps(){
    delta_timestamps = false;
}

void
_do_complaint(int priority, const std::vector<std::string>& vs){
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
    bool keep = level < LOG_ERR || ( rate < max_hourly_rate ) || u01(e) < max_hourly_rate / (vs.size() * rate);
    // We could write another dozen lines of code to try to report on
    // the discards and the rate ... OR NOT... We're putting a
    // sequence-number on every group of messages. We can always look
    // for gaps in the sequence after the fact...
    if(!keep)
        return;

    // Finally - we  can actually deliver our complaints!

    for(const auto& s : vs){
        the_channel().send(level, s);
    }
}
    
static std::atomic<int> seq_atomic;
    
std::vector<std::string>
_whatnest (int priority, const std::string& pfx, const std::exception* ep){
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

// FIXME - We have two nearly identical implementations of _whatnest.
// That's one too many.  Refactor!
std::vector<std::string>
_whatnest (int priority, const std::string& pfx, char const* const* beg, char const* const* end){
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

}

#include "core123/log_channel.hpp"
#include "core123/sew.hpp"
#include "core123/syslog_number.hpp"
#include "core123/strutils.hpp"
#include <cstdio>
#include <stdexcept>
#include <mutex>

// The logic is messy.  It maintains the invariant that no more than
// one of the following can be true:
//
//   dest_syslog == true
//   dest_fd != -1
//
// It's OK for both to be false, in which case we don't do any output
// at all. 

namespace core123{

log_channel&
log_channel::operator=(log_channel&& rvr){
    std::lock_guard<std::mutex> lg(mtx);
    dest_syslog = rvr.dest_syslog;
    dest_lev = rvr.dest_lev;
    dest_fac = rvr.dest_fac;
    dest_fd = rvr.dest_fd;
    dest_opened = rvr.dest_opened;
    rvr.dest_opened = false;
    return *this;
}

void log_channel::_close() /* private */{
    dest_syslog = false;
    dest_lev = 0;
    dest_fac = 0;
    if(dest_opened)
        sew::close(dest_fd);
    dest_fd = -1;
    dest_opened = false;
}    

void log_channel::open(const std::string& dest, int mode){
    std::lock_guard<std::mutex> lg(mtx);
    // Call _close to preserve the dest_ invariant by falsifying all
    // pre-conditions.  Then they turn on no more than one.
    _close();
    // And then turning at most one of them on.
    if(dest.empty() || dest == "%none"){
        return;
    }else if(core123::startswith(dest, "%syslog")){
        parse_dest_priority(dest); // might throw
        dest_syslog = true;
    }else if(dest == "%stderr"){
        dest_fd = fileno(stderr);
    }else if(dest == "%stdout"){
        dest_fd = fileno(stdout);
    }else if(dest[0] == '%'){
        throw std::runtime_error("log_channel::open: Unrecognized %destination: "  + dest);
    }else{
        dest_fd = sew::open(dest.c_str(), O_CLOEXEC|O_WRONLY|O_APPEND|O_CREAT, mode);
        dest_opened = true;
    }
}

void log_channel::send(int level, str_view sv) const {
    // Should we throw on error?
    // level is only used with syslog.  Oversight?  Inadequate  API?
    // Lack of imagination?
    if(sv.size() == 0)
        return;
    std::lock_guard<std::mutex> lg(mtx);
    if(dest_syslog){
        // if level is unspecified (-1) use dest_lev.  If it
        // is specified, silently mask off any non-level bits.
        auto lev = (level == -1) ? dest_lev : (level&0x7);
        // no autonewline for syslog
        ::syslog(dest_fac | lev, "%.*s", int(sv.size()), sv.data());
    }else if(dest_fd >= 0){
        // autonewline with writev
        struct iovec iov[2] = {{const_cast<char*>(sv.data()), sv.size()},
                               {const_cast<char*>("\n"), 1}};
        int iovcnt = sv[sv.size()-1] == '\n' ? 1 : 2;
        sew::writev(dest_fd, iov, iovcnt);
    }
    // else, quietly do nothing 
}

// in syslog(3) man pages, priority = facility | level.
// In rfc5424, the BNF calls priority PRIVAL and
// more-or-less says that:
//   PRIVAL = (FACILITY*8 + SEVERITY)
// But in syslog.h, the levels/severities are called 'priorities', and
// can be extracted with LOG_PRIMASK.
//
// Bottom line:  everyone agrees that one part is called
// the 'facility'.  The other part is called different
// things in different sources.  *WE* call it a level.
// And we call the whole thing a 'priority'.

// See comments in log_channel.hpp specifying the format.
void log_channel::parse_dest_priority(const std::string& destarg) {
    // Sigh... This is awfully fiddly...
    size_t percent = sizeof("%syslog") - 1;
    if( destarg.size() < percent )
        throw std::logic_error("expected to see %syslog");
    dest_fac = LOG_USER;
    dest_lev = LOG_NOTICE;
    if( destarg.size() == percent )
        return;  // destarg is just "%syslog"
    if(destarg[percent] != '%')
        throw std::runtime_error("expected a %");
    auto args = svsplit_exact(destarg, "%", percent+1);
    if(args.size() > 2 || args.size() < 1)
        throw std::runtime_error("expected either one or two '%arguments' after %syslog");
    for(str_view arg : args){
        int n = syslog_number(std::string(arg)); // might throw
        if((n & LOG_PRIMASK) == n)
            dest_lev = n;
        else
            dest_fac = n;
    }        
}

} // namespace core123

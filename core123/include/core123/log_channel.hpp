#pragma once

// log_channel - A *minimal* interface that allows one to
// 'send' individual pre-formatted log messages (string_view) to
// either a named file, a syslog facility or stdout or stderr.
//
// The crucial feature is that the output channel can be requested
// *by name*, which can be a plain filename (absolute or relative)
// or one of these %special forms:
//
//   %none - no output is written
//   %stdout - output is written to file descriptor 1
//   %stderr - output is written to file descriptor 2
//   %syslog - output is to syslog(LOG_USER, with level LOG_NOTICE
//   %syslog%LOG_LEVEL%LOG_FACILITY - one or both of the facility and
//        level may be specified, in either order.  The format is
//        strict.  Facility and level must be spelled correctly with
//        no additional whitespace or punctuation permitted.  If the
//        level is unspecified, it is LOG_NOTICE.  If the facility is
//        unspecified, it is LOG_USER.
//
// In addition, when the output is *not* to syslog, a newline
// will be automatically appended to any record that does not
// end with a newline.
//
// The reopen() method closes and then reopens the current destination.
// It may be useful for log rotation.
//
// Thread-safety: send(), open() and reopen() are thread-safe.  I.e.,
// they may be called "concurrently" by multiple threads and the
// effect will be as if they were called in some (unspecified) order.
// On the other hand the caller must guarantee that the log_channel is
// neither destroyed nor std::move-ed while any other thread is doing
// send(), open() or reopen().

// See diag.hpp or complaints.hpp for usage examples.

#include "str_view.hpp"
#include "sew.hpp"
#include "syslog_number.hpp"
#include "strutils.hpp"
#include <string>
#include <mutex>
#include <stdexcept>

namespace core123{
struct log_channel{
    mutable std::mutex mtx;
    bool dest_syslog = false;
    int dest_fd = -1;
    int dest_fac = 0;
    int dest_lev = 0;
    bool dest_opened = false;
    std::string opened_dest;
    int opened_mode;
    log_channel(){}
    // copy constructor and copy assignment would confuse the
    // semantics of dest_fd and dest_owned!
    log_channel(const log_channel&) = delete;
    log_channel& operator=(const log_channel&) = delete;
    // but move constructor and move-assignment are sensible.
    log_channel(log_channel&& rvr){ *this = std::move(rvr); }
    log_channel& operator=(log_channel&& rvr){
        std::lock_guard<std::mutex> lg(mtx);
        dest_syslog = rvr.dest_syslog;
        dest_lev = rvr.dest_lev;
        dest_fac = rvr.dest_fac;
        dest_fd = rvr.dest_fd;
        dest_opened = rvr.dest_opened;
        rvr.dest_opened = false;
        return *this;
    }

    log_channel(const std::string& dest, int mode){
        open(dest, mode);
    }
    ~log_channel() try {
        _close();
    }catch(...){}

    void open(const std::string& dest, int mode){
        std::lock_guard<std::mutex> lg(mtx);
        // Call _close to preserve the dest_ invariant by falsifying all
        // pre-conditions.  Then they turn on no more than one.
        _close();
        // And then turning at most one of them on.
        opened_dest = dest;
        opened_mode = mode;
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

    void reopen(){
        std::unique_lock<std::mutex> lk(mtx);
        auto dest = opened_dest;
        auto mode = opened_mode;
        lk.unlock();
        open(dest, mode);
    }

    void close(){ open("%none", 0); }

    void send(int level, str_view sv) const{
        // Should we throw on error?
        // level is only used with syslog.  Oversight?  Inadequate  API?
        // Lack of imagination?
        std::lock_guard<std::mutex> lg(mtx);
        if(sv.size() == 0)
            return;
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

    void send(str_view sv) const { send(-1, sv); }
private:
    void _close(){ // assumes lock is already held
        dest_syslog = false;
        dest_lev = 0;
        dest_fac = 0;
        if(dest_opened)
            sew::close(dest_fd);
        dest_fd = -1;
        dest_opened = false;
    }
    
    // A note on terminology:
    // In syslog(3) man pages, priority = facility | level.  In
    // rfc5424, the BNF calls priority PRIVAL and more-or-less says
    // that:
    //   PRIVAL = (FACILITY*8 + SEVERITY)

    // But in syslog.h, the levels/severities are called 'priorities',
    // and can be extracted with LOG_PRIMASK.
    //
    // Bottom line: everyone agrees that one part is called the
    // 'facility'.  The other part is called different things
    // ('level', 'severity' or 'priority') by different people.  *WE*
    // call it a level.  And we call the whole thing a 'priority',
    // both of which agree with man syslog(3).
    void parse_dest_priority(const std::string& destarg){
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

};
} // namespace core123

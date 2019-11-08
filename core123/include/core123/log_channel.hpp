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
#include <string>
#include <mutex>

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
    log_channel& operator=(log_channel&&);

    log_channel(const std::string& dest, int mode){
        open(dest, mode);
    }
    ~log_channel() try {
        _close();
    }catch(...){}
    void open(const std::string& dest, int mode);
    void reopen();

    void close(){ open("%none", 0); }
    void send(int level, str_view) const;
    void send(str_view sv) const { send(-1, sv); }
private:
    void _close(); // assumes lock is already held
    void parse_dest_priority(const std::string&);
};
} // namespace core123

#pragma once
#include <string>
#include <map>
#include <stdexcept>
#include <syslog.h>

namespace core123{
// syslog_number - a little helper to convert strings to numbers, e.g.,
//   syslog_number("LOG_INFO") -> LOG_INFO = 6
// making it easy to convert argv and env-vars to syslog symbols.

// syslog.h doesn't have a 'level' for NONE.  So we invent one that
// "works" when used as LOG_UPTO(LOG_LEVEL_NONE).  Note that LOG_EMERG is
// also 0.  So calling
//    setlogmask(LOG_UPTO(LOG_LEVEL_NONE))
// still passes syslogs with 'EMERG' priority.  It appears that
// this is the best we can do.  The syslogmask man page says
// that if its argument is zero, the mask is unchanged.
//
// Furthermore, note that LOG_KERN is also 0.  So if one
// incorrectly sets the facility to LOG_LEVEL_NONE with:
//     openlog(..., LOG_LEVEL_NONE)
// that's the same as setting the facility to LOG_KERN.  According to
// the man pages, LOG_KERN messages can't be generated from user
// processes.  A quick experiment suggests that openlog(..., 0) leaves
// the facility unchanged (its default value is LOG_USER).  The macro
// is called LOG_LEVEL_NONE to discourage accidental misuse as a
// facility.
#define LOG_LEVEL_NONE 0

inline int syslog_number(const std::string& s){
#define _sl_Enum(name) {std::string(#name), name}
    static const std::map<std::string, int> syslog_symbols = {
    // options
    _sl_Enum(LOG_CONS),
    _sl_Enum(LOG_NDELAY),
    _sl_Enum(LOG_NOWAIT),
    _sl_Enum(LOG_PERROR),
    _sl_Enum(LOG_PID),
    // facilities
    _sl_Enum(LOG_AUTH),
    _sl_Enum(LOG_AUTHPRIV),
    _sl_Enum(LOG_CRON),
    _sl_Enum(LOG_DAEMON),
    _sl_Enum(LOG_FTP),
    _sl_Enum(LOG_KERN),
    _sl_Enum(LOG_LOCAL0),
    _sl_Enum(LOG_LOCAL1),
    _sl_Enum(LOG_LOCAL2),
    _sl_Enum(LOG_LOCAL3),
    _sl_Enum(LOG_LOCAL4),
    _sl_Enum(LOG_LOCAL5),
    _sl_Enum(LOG_LOCAL6),
    _sl_Enum(LOG_LOCAL7),
    _sl_Enum(LOG_LPR),
    _sl_Enum(LOG_MAIL),
    _sl_Enum(LOG_NEWS),
    _sl_Enum(LOG_SYSLOG),
    _sl_Enum(LOG_USER),
    _sl_Enum(LOG_UUCP),
    // levels
    _sl_Enum(LOG_EMERG),  // N.B. On systemd systems, this sends to wall by default
    _sl_Enum(LOG_CRIT),
    _sl_Enum(LOG_ERR),
    _sl_Enum(LOG_WARNING),
    _sl_Enum(LOG_NOTICE),
    _sl_Enum(LOG_INFO),
    _sl_Enum(LOG_DEBUG),
    _sl_Enum(LOG_LEVEL_NONE) // see comments in useful.h
    };
#undef _sl_Enum

    auto p = syslog_symbols.find(s);
    if( p == syslog_symbols.end() )
        throw std::runtime_error(s + " is not a recognized syslog symbol");
    return p->second;
}
} // namespace core123

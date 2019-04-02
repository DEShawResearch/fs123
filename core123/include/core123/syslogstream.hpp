#pragma once

#include <string>
#include <syslog.h>
#include <iostream>
#include <sstream>
#include <syslog.h>

// This is a bit cleaner and smaller than the syslogstream module.
// The flush-in-destructor feature is an improvement, and encourages
// thread-safety.

// syslogstream() - an ostream with output directed to the syslog.
// The output is flushed to syslog when:
//    o the syslogstream is destroyed.
//    o flush or endl manipulators are inserted or the flush() method
//      is called.
//
// For thread-safety, it's best to create a one-shot syslogstream "on
// the stack" as-needed, rather than to rely on a global extern
// syslogstream.  I.e., say:
//
//   syslogstream(LOG_INFO) << "I have something to say.  value=" << value;
//
// The constructor argument is the 'priority', i.e., FACILITY|LEVEL.
// Following syslog conventions, if the FACILITY is unspecified, the
// value previously provided to openlog is used or LOG_USER if openlog
// hasn't been called.  Consult the syslog man page for the details of
// facilities, levels, priorities, openlog, closelog and more options.
// See also man setlogmask.
//
//      openlog(argv[0], 0,  LOG_LOCAL3);
//      syslogstream(LOG_INFO) << "All is well";
//      syslogstream(LOG_ERR) << "Houston, we have a problem. x=" << x;
//
//      // It's also possible to keep a syslogstream around for re-use,
//      // but then you must explicitly flush it to see the output.
//      syslogstream sls(LOG_INFO);
//      sls << "Hello" << std::flush;
//      // Messages aren't sent to syslog until it's flushed:
//      sls << "Multipart ... ";
//      sls.reprioritize(LOG_NOTICE);  // change priority before flushing
//      sls << "message" << std::flush;
//     
namespace core123{
struct syslogbuf : public std::stringbuf{
protected:
    int priority;
    virtual int sync() { 
        // Is the stringbuf empty?
        if( pptr() != epptr() ){
            ::syslog(priority, "%s", str().c_str());
            str(std::string());
        }
        return 0;
    }

public:
    syslogbuf(int priority_) : priority(priority_){}
    void reprioritize(int priority_) { priority = priority_; }
    virtual ~syslogbuf(){
        sync();
    }
};

struct syslogstream : public std::ostream{
    syslogbuf m_syslogbuf;
    syslogstream(int priority) : std::ostream(&m_syslogbuf), m_syslogbuf(priority) {}
    void reprioritize(int priority) { m_syslogbuf.reprioritize(priority); }
};

} // namespace core123

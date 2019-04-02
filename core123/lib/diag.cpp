#include "core123/diag.hpp"
#include "core123/str_view.hpp"
#include "core123/named_ref.hpp"
#include "core123/pathutils.hpp"
#include "core123/log_channel.hpp"
#include <sstream>
#include <cstring>
#include <string>
#include <iostream>
#include <iomanip>
#include <map>
#include <tuple>
#include <cstdlib>
#include <fcntl.h>
#if defined(__linux__)
#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>
#else
#include <thread>
#endif
using namespace std;

namespace core123{

std::recursive_mutex _diag_mtx;

} // namespace core123::diag

namespace {

// The names() function *could* be in the public core123:: namespace.  Do
// we want that?
core123::named_ref_space<int>& names(){
    // A new pointer?  This leaks!  Yes.  This is intentional.  It
    // neatly avoids the equally bad static destructor fiasco when
    // everything gets torn down.
    static core123::named_ref_space<int>* thenames = new core123::named_ref_space<int>;
    return *thenames;
}
    
// streams...  Ugh
struct fancy_stringbuf : public std::stringbuf{
    fancy_stringbuf() : std::stringbuf(std::ios_base::out) {}
    core123::str_view sv(){
        return {pbase(), size_t(pptr()-pbase())};
    }
    void reset(){
        setp(pbase(), epptr()); // has the side-effect of setting pptr to new_pbase
    }
};
fancy_stringbuf ostringbuf;
std::ostream os(&ostringbuf);

void set_opt_defaults(){
    core123::diag_opt_tstamp = false;
    core123::diag_opt_tid = false;
    core123::diag_opt_srcdir = false;
    core123::diag_opt_srcfile = false;
    core123::diag_opt_srcline = false;
    core123::diag_opt_func = true;
    core123::diag_opt_why = true;
    core123::diag_opt_newline = false;
    core123::diag_opt_flood = false;
}

// clear_names:  zero out all the diag_names:
void clear_names(){
    // can't just say kv.second = 0 because getmap
    // returns a const map.  So we jump through the
    // declare hoop to get something assignable.
    for(auto& kv : names().getmap())
        (int&)names().declare(kv.first) = 0;
}

struct env_initializer{
    env_initializer() try {
        const char *p;
        core123::set_diag_names( (p=::getenv("CORE123_DIAG_NAMES")) ? p : "");
        core123::set_diag_opts( (p=::getenv("CORE123_DIAG_OPTS")) ? p : "");
        core123::set_diag_destination( (p=::getenv("CORE123_DIAG_DESTINATION")) ? p : "%stderr");
    }catch(std::exception &e){
        std::cerr << "WARNING: an error was encountered initializing the diag.hpp subsystem from environment variables: " << e.what() << std::endl;
    }
};

core123::log_channel the_log_channel("%stderr", 0);
env_initializer _at_startup_;

} // namespace <anon>

namespace core123{

std::ostream& diag_intermediate_stream = os;

named_ref<int> diag_name(const std::string& name, int initial_value){
    return names().declare(name, initial_value);
}

// str is expected to be a colon-separated list of key[=decimal_value]
// tokens, each of which sets the diagnostic level of 'key' to the
// given decimal value (1 if the value is unspecified).
void set_diag_names(const std::string& str, bool clear_before_set){
    if(clear_before_set)
        clear_names();
    string::size_type start, colon;
    colon= -1;
    do{
        start = colon+1;
        colon = str.find(':', start);
        string tok=str.substr(start, colon-start);
        string::size_type idx;
        string skey;
        int lev;
        if(tok.empty())  // what about whitespace?
            break;
        if( (idx=tok.find('=')) != string::npos ){
            // key=level
            skey = tok.substr(0, idx);
            // If there's a parse error?  Leave lev=default
            lev = 1;
            sscanf(tok.substr(idx+1).c_str(), "%d", &lev);
        }else{
            // key
            skey = tok;
            lev = 1;
        }
        (int&)names().declare(skey) = lev;
    }while( colon != string::npos );
}

// getnames:  the "inverse" of setnames.  The string returned can
// be fed back into setlevels.
std::string get_diag_names(bool showall){
    const char *sep = "";
    std::ostringstream oss;
    for(const auto& kv : names().getmap()){
        if(showall || kv.second != 0){
            oss << sep << kv.first << "=" << kv.second;
            sep = ":";
        }
    }
    return oss.str();
}

// N.B.  some of these are assigned non-zero defaults in set_opt_defaults, which
// gets called *very* early (during static initialization).
bool diag_opt_tstamp;
bool diag_opt_tid;
bool diag_opt_srcdir;
bool diag_opt_srcfile;
bool diag_opt_srcline;
bool diag_opt_func;
bool diag_opt_why;
bool diag_opt_newline;
bool diag_opt_flood;

// set_opts:  read a colon separated list of tokens and
//  set the fmt options accoringly
void set_diag_opts(const std::string& s, bool restore_defaults_before_set){
    if(restore_defaults_before_set)
        set_opt_defaults();
    string::size_type start, colon;
    colon = -1;
    do{
        start = colon + 1;
        colon = s.find(':', start);
        string tok = s.substr(start, colon-start);
        bool negate = core123::startswith(tok, "no");
        if(negate)
            tok = tok.substr(2);
        if(tok == "tstamp")
            diag_opt_tstamp = !negate;
        else if(tok == "tid")
            diag_opt_tid = !negate;
        else if(tok == "srcdir")
            diag_opt_srcdir = !negate;
        else if(tok == "srcfile")
            diag_opt_srcfile = !negate;
        else if(tok == "srcline")
            diag_opt_srcline = !negate;
        else if(tok == "func")
            diag_opt_func = !negate;
        else if(tok == "why")
            diag_opt_why = !negate;
        else if(tok == "newline")
            diag_opt_newline = !negate;
	else if(tok == "flood")
	    diag_opt_flood = !negate;
    }while( colon != string::npos );
}

std::string get_diag_opts(){
    std::ostringstream oss;
    const char *sep = "";
#define FOO(opt)                \
    oss << sep;                 \
    if(!diag_opt_##opt) oss << "no"; \
    oss << #opt;                \
    sep = ":"                  
    FOO(tstamp);
    FOO(tid);
    FOO(srcdir);
    FOO(srcfile);
    FOO(srcline);
    FOO(func);
    FOO(why);
    FOO(newline);
    FOO(flood);
#undef FOO
    return oss.str();
}

std::ostream& _diag_before(const char* k, const char *file, int line, const char *func){
    if(diag_opt_tstamp){
        // N.B. formatting the timestamp adds about 1.0 musec on a 2017 intel core!?
        // Without a timestamp, it takes about 0.2 musec.
        using namespace std::chrono;
        // to_time_t might round.  We don't want that...
        // duration_cast always rounds toward zero.
        auto now_musec = duration_cast<microseconds>( system_clock::now().time_since_epoch() ).count();
        time_t now_timet = now_musec/1000000;
        auto musec = now_musec%1000000;
        // On Linux (glibc 2.17) localtime does
        // stat("/etc/localtime") *every* time it's called.  This
        // can be circumvented by setting the TZ environment
        // variable:
        // https://blog.packagecloud.io/eng/2017/02/21/set-environment-variable-save-thousands-of-system-calls/
        // For glibc, TZ is described here:
        // https://www.gnu.org/software/libc/manual/html_node/TZ-Variable.html
        //
        // N.B.  On CentOS7, man timezone(3) IS NOT CORRECT.  It
        // incorrectly suggests that setting TZ=: would read a
        // file /usr/share/zoneinfo/localtime.
        //
        // According to the glibc link above, "the default time
        // zone is like the specification ‘TZ=:/etc/localtime’ (or
        // ‘TZ=:/usr/local/etc/localtime’, depending on how the
        // GNU C Library was configured)".  CentOS7 seems to use
        // /etc/localtime, so let's use that and hope for the
        // best.  If it happens that /etc/localtime does not
        // exist, experimentation suggests that glibc's localtime
        // will report GMT.  Other operating systems (e.g., MacOS,
        // BSD) may behave differently :-(.
        ::setenv("TZ", ":/etc/localtime", 0);
        struct tm *now_tm = ::localtime(&now_timet); // how slow  is localtime?   Do we care?
        if(now_tm){
            // N.B.  this is actually faster than stringprintf (gcc6, 2017)
            auto oldfill = os.fill('0');
            // E.g., "19:09:51.779321 mount.fs123p7.cpp:862 [readdir] "
            os << std::setw(2)  << now_tm->tm_hour << ':' << std::setw(2) << now_tm->tm_min << ":" << std::setw(2) << now_tm->tm_sec << "." << std::setw(6) << musec << ' ';
            os.fill(oldfill);
        }
    }
    if(diag_opt_tid){
#if defined(__linux__)
        os << '[' << ::syscall(SYS_gettid) << "] ";
#else
        std::ios::fmtflags oldflags(os.flags());
        os << std::hex << '[' << std::this_thread::get_id() << "] ";
        os.flags(oldflags);
#endif
    }
    if(diag_opt_srcfile || diag_opt_srcdir){
        std::string filepart;
        std::string dirpart;
        std::tie(filepart, dirpart) = core123::pathsplit(file);
        if(diag_opt_srcdir)
            os << dirpart << '/';
        if(diag_opt_srcfile)
            os << filepart;
    }
    if(diag_opt_srcline)
        os << ':' << line;
    if(diag_opt_func)
        os << func << "() ";
    if(diag_opt_why)
        os << "[" << k << "] ";
    return os;
}

void _diag_after(const char* /*k*/, const char */*file*/, int /*line*/, const char */*func*/){
    if(diag_opt_newline)
        ostringbuf.sputc('\n');
    the_log_channel.send(ostringbuf.sv());
    ostringbuf.reset();
}

void set_diag_destination(const std::string& diagfname, int mode){
    std::lock_guard<std::recursive_mutex> lk(_diag_mtx);
    the_log_channel.open(diagfname, mode);
}

} // namespace core123



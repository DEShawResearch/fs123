#pragma once
#ifdef DIAG
#error You are probably including both diag.h and diag.hpp.  Bad idea.
#endif

/*! \brief Some tools for inserting diagnostics into code.
     \mainpage Diag

  \author John Salmon <John.Salmon@deshaw.com>,

  Copyright:  2018 all rights reserved

  Basic Usage:

   #include <fs123/diag.hpp>
   ...
   DIAG(bool_expr, "anything " << formatable << " using the stream insertion operator(<<)" 
                   << whoopee << "\n");
   DIAGf(bool_expr, "old-school formating with %s %f etc\n", "percents", 3.1415);

  The bool_expr can be any boolean expression.  DIAG and DIAGf are macros, carefully
  constructed so that the second through last arguments are *not evaluated* if the
  bool_expr is false.  Thus, they're safe even in tight loops in high-performance
  code - as long as the bool_expr quickly returns false in the normal case.

  The diag namespace allows one to declare 'named integer' values that
  satisfy the quick-to-evaluate criterion, and that can be set globally,
  either by the program explicitly or from the environment.  E.g.

    // at file scope, function scope or block scope:
    auto _foo = core123::diag_name("foo");

    // anywhere in the code
    DIAG(_foo, "I have something interesting to say about foo");

  core123::diag_name returns a named_ref<int>.  It is interconvertible to 
  and from plain int.  So you could say:

    DIAG(_foo>2, "I have something to say, but only if you're *very* interested in 'foo'");

  You can use them logical expressions, e.g.,

    DIAG(_foo || _bar, "Interesting for foo and bar afficionados");

  You can assign values to named_refs with =, +=, etc.  E.g.,

    _foo = 3;
    _foo -= 2;

  But the real power of named_refs is that they are "named" by
  strings, so they can be controlled by strings in the environment or
  on the command line.  The function:

    core123::set_diag_names(const std::string& new_names, bool clear_before_set = true);

  can be used to set several named_refs at once; it resets the values
  of all existing names to 0 if and only if clear_before_set is true.
  Furthermore, code equivalent to:

    core123::set_diag_names((p=::getenv("CORE123_DIAG_NAMES")) ? p : "");

  is executed in the constructor of the_diag() singleton.  Thus,
  diagnostic names can be set in the environment, requiring no
  application-level code.

  core123::set_diag_names should be called with a colon-separated list of
  name[=value] pairs.  E.g.,

    core123::set_diag_names("foo:bar=3");

  or equivalently:

    export CORE123_DIAG_NAMES="foo:bar=3"

  would set diagnostic named_refs with name "foo" to 1 and those with
  name "bar" to 3.

  Note that it's probably a good idea to adopt a naming convention
  like the one used here: named_ref objects declared with the name
  "XXX" are called _XXX in the code.  This convention is not enforced,
  but it will make your code easier to read and understand.

  If any files of a program using diag are compiled with the
  preprocessor symbol CORE123_DIAG_FLOOD_ENABLE defined, then all
  diags in those files can be turned on by setting the
  core123::diag_opt_flood flag directly or via core123::set_diag_opts() or
  the DIAG_OPTS enviroment variable. It will likely produce
  a *LOT* of output, but is an easy way to test diag code,
  as well as quickly look into what is going on under the
  hood and empirically figure out what names one might want
  to selectively turn on.  Since CORE123_DIAG_FLOOD_ENABLE adds one
  more boolean to every macro, it has a little bit of extra
  overhead in the "fast" path which is why it defaults to off.
  
  Scope?
  ------

  The declaration:

    auto _foo = core123::diag_name("foo");

  can be made at file scope, function scope, or block scope.  If it's
  at file scope, it can be static or in an anonymous namespace.

  So what *should* it be?

  If you can be *sure* that their values won't be read during static
  initialization, then the easiest thing to do is to declare the
  "diagnostic names" used in a source file at file scope, near the top
  of the file.  E.g.,

  #include <this>
  #include <that>
  #include <core123/diag.hpp>

  using namespace core123;
  static auto _foo = diag_name("foo");
  static auto _bar = diag_name("bar");
  // or, equivalently, if you prefer the anonymous namespace
  namespace {
      auto _foo = diag_name("foo");
      auto _bar = diag_name("bar");
  }

  File-scoped statics are always at risk of encountering "static
  initialization order" issues.  If there is any chance that an object
  might be referenced at static initialization time (e.g., if it's
  used in a function that's called during the initialization of
  another object, defined in another file), then file-scoped
  initializers like the ones above are troublesome.  In such cases, a
  function-scoped static is a safe choice.

  // someMember might be called during static initialization 
  // of a someType defined in some other source file.
  someType::someMember(){ 
      static auto _foo = diag_name("foo");
      ...
      DIAG(_foo, "Did somebody say foo?");
  }

  The only down side is that the definition has to be repeated in each
  function where the name is used.

  N.B.  Library writers should generally assume that the library's
  functions and methods may be called during static initialization.
  Therefore library code should generally call diag_name at
  function scope, and avoid using it at file scope.


  Where does the output go?
  ---------------------

   You can control the ultimate destination of your diagnostics
   with the the function:

      set_diag_destination(const std::string& dest, int mode=0666);

   Furthermore, code like this will be executed at static initialization time:

    set_diag_destination((p=::getenv("CORE123_DIAG_DESTINATION")) ? p : "%stderr");

   so the destination of diagnostics can be set in the environment,
   requiring no application-level code.

   If the string argument to core123::set_diag_destination is not one of the
   special forms (see below), then it is the name of a regular file,
   opened with flags O_WRONLY|O_APPEND|O_CREAT, and all diagnostics
   are sent to it.  If the open fails, diagnostics will be discarded.

   The following special forms are understood:

     "%none"   - messages will be discarded
     "%syslog" - messages are sent to syslog
     "%stderr" - messages are sent to file descriptor 2 (stderr)
     "%stdout" - messages are sent to file descriptor 1 (stdout)
     ""        - empty string, equivalent to "%none"

   The %syslog destination allows modifiers.  I.e., these do pretty
   much what you'd expect:

     %syslog              // with facility LOG_USER and level LOG_NOTICE
     %syslog%LOG_WARNING // with facility LOG_USER
     %syslog%LOG_LOCAL6  // with level LOG_NOTICE
     %syslog%LOG_WARNING%LOG_LOCAL6

  Formatting the diagnostics:
  ---------------------------

  The formatting is controlled by a few booleans in the singleton the_diag():

   the_diag().opt_tstamp - include a microsecond timestamp in each output record
   the_diag().opt_tid - include the thread-id of the calling process in each output record
   the_diag().opt_srcdir - include the directory part of __FILE__ in each output record
   the_diag().opt_srcfile - include the last component of __FILE__ in each output record
   the_diag().opt_srcline - include the value of __LINE__ in each output record
   the_diag().opt_func   - include the value of __func__ in each output record
   the_diag().opt_why    - include the stringified bool_expr in every output record
   the_diag().opt_newline - unconditionally add a newline to every output record
   the_diag().opt_flood - turns on all DIAG/DIAGf macros unconditionally

  opt_func and opt_why are true by default.  All others are
  false by default.

  You can set the opt_ options in your code by assigning directly to the
  members of the_diag().  They can also be set with a string argument:

     set_diag_opts(const std::string& opts, bool reset_before_set=true);

  The string argument is expected to contain colon or
  whitespace-separated names of format options without the
  opt_ prefix e.g. "tstamp:tid:nonewline".  Options can be
  preceded by "no" to explicitly disable an option.  All
  options are reset to their default values before parsing
  the opts string if and only if reset_before_set is true.
  The following code will be executed in the_diag() constructor:

      set_diag_opts(::getenv("CORE123_DIAG_OPTS") || "");

  so options can be set in the environment, requiring no
  application-level code.

  MAJOR REVISIONS
   - core123 
        o complete overhaul of functionality and API.
   - Version 5.0: Unify CDIAG and DIAG
   - Version 4.0: Deprecate string keys in favor of bona fide DiagKeys.
   - Version 2.0: Diag is a class, each instance has a m_ostream. Slightly
       faster predicate (integer compare, no map lookup). Supports diaglog
       and syslog derived classes. Better control of meta data.
   - Version 1.0: one global diagstream.

 */


#include "str_view.hpp"
#include "strutils.hpp"
#include "named_ref.hpp"
#include "log_channel.hpp"
#include "pathutils.hpp"
#include "svstream.hpp"
#include <sstream>
#include <string>
#include <iostream>
#include <iomanip>
#include <mutex>
#include <fcntl.h>
#if defined(__linux__)
#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>
#else
#include <thread>
#endif

/* Define base macros depending on NODIAG */
#ifdef NODIAG
#define DIAGloc(key, lev, ...)
#else

#ifdef __GNUC__  // __builtin_expect was introduced in 2.96.
// Gcc let's us hint to the compiler that a test is unlikely,
// so that it emits straightline code for the else case, and
// a jump for the if case.
#define __diag_unlikely(x) __builtin_expect(x, 0)
#else
#define __diag_unlikely(x) x
#endif

#ifdef CORE123_DIAG_FLOOD_ENABLE
/* enables the diag_all boolean shortcircuit.  With gcc6, on x86_64
   simply enabling this option slows down DIAG(false, ...) by a factor
   of two (from 0.4ns to 0.8ns) even when opt_flood is false.  On the
   other hand, it is convenient to have the capability for testing a
   program to ensure all diags work and produce meaningful output
   without having to grep for and turn on all diags manually, and
   0.4ns is pretty quick, even if it is a factor of two.
*/
#define _diag_flood core123::the_diag().opt_flood
#else
#define _diag_flood 0
#endif /* DIAG_FLOOD_ENABLE */

#define DIAGloc(BOOL, _file, _line, _func, _expr) do{                   \
        if( __diag_unlikely(_diag_flood || bool(BOOL)) ){     \
            core123::diag_t& td = core123::the_diag();                  \
            std::lock_guard<std::recursive_mutex> __diag_lg(td._diag_mtx); \
            td._diag_before( #BOOL , _file, _line, _func) << _expr; \
            td._diag_after( #BOOL , _file, _line, _func);        \
        }                                                               \
    }while(0)

#endif /* NODIAG */

#define DIAG(BOOL, expr)                        \
    DIAGloc(BOOL, __FILE__, __LINE__, __func__, expr)

#define DIAGf(BOOL, ...) \
    DIAGloc(BOOL, __FILE__, __LINE__, __func__, core123::fmt(__VA_ARGS__))

// Some people strongly object to briefly releasing shift key to type lower-case 'f':
#define DIAGF DIAGf
// And there's a lot of old code using DIAGfkey and DIAGkey that
// will "just work" if we provide:
#define DIAGfkey DIAGf
#define DIAGkey DIAG

namespace core123{

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

struct diag_t{
    // "methods" for working with the names in the named_ref_space.
    named_ref<int> diag_name(const std::string& name, int initial_value = 0){
        return thenames->declare(name, initial_value);
    }

    // set_diag_names: str is expected to be a colon-separated list of
    // key[=decimal_value] tokens, each of which sets the diagnostic
    // level of 'key' to the given decimal value (1 if the value is
    // unspecified).
    void set_diag_names(const std::string& str, bool clear_before_set = true){
        using std::string;
        if(clear_before_set)
            clear_names();
        string::size_type start, colon;
        colon=0; colon -= 1; // i.e., colon=-1, without a warning
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
            (int&)thenames->declare(skey) = lev;
        }while( colon != string::npos );
    }

    // get_diag_names:  the "inverse" of set_diag_names.  The string returned can
    // be fed back into set_diag_names.
    std::string get_diag_names(bool showall=false){  // showall says whether to report zero-valued keys.
        const char *sep = "";
        std::ostringstream oss;
        for(const auto& kv : thenames->getmap()){
            if(showall || kv.second != 0){
                oss << sep << kv.first << "=" << kv.second;
                sep = ":";
            }
        }
        return oss.str();
    }

    void set_diag_destination(const std::string& diagfname, int mode=0666){ // umask is respected
        std::lock_guard<std::recursive_mutex> lk(_diag_mtx);
        logchan.open(diagfname, mode);
    }

    // set_diag_opts: read a colon separated list of tokens and set
    //  the formatting options accordingly.
    void set_diag_opts(const std::string& s, bool restore_defaults_before_set = true){
        using std::string;
        if(restore_defaults_before_set)
            set_opt_defaults();
        string::size_type start, colon;
        colon=0; colon -= 1; // i.e., colon=-1, without a warning
        do{
            start = colon + 1;
            colon = s.find(':', start);
            string tok = s.substr(start, colon-start);
            bool negate = core123::startswith(tok, "no");
            if(negate)
                tok = tok.substr(2);
            if(tok == "tstamp")
                opt_tstamp = !negate;
            else if(tok == "tid")
                opt_tid = !negate;
            else if(tok == "srcdir")
                opt_srcdir = !negate;
            else if(tok == "srcfile")
                opt_srcfile = !negate;
            else if(tok == "srcline")
                opt_srcline = !negate;
            else if(tok == "func")
                opt_func = !negate;
            else if(tok == "why")
                opt_why = !negate;
            else if(tok == "newline")
                opt_newline = !negate;
            else if(tok == "flood")
                opt_flood = !negate;
        }while( colon != string::npos );
    }

    std::string get_diag_opts(){
        std::ostringstream oss;
        const char *sep = "";
#define _DIAG_OPT(opt)                          \
        oss << sep;                             \
        if(!opt_##opt) oss << "no";             \
        oss << #opt;                            \
        sep = ":"                  
        _DIAG_OPT(tstamp);
        _DIAG_OPT(tid);
        _DIAG_OPT(srcdir);
        _DIAG_OPT(srcfile);
        _DIAG_OPT(srcline);
        _DIAG_OPT(func);
        _DIAG_OPT(why);
        _DIAG_OPT(newline);
        _DIAG_OPT(flood);
#undef _DIAG_OPT
        return oss.str();
    }

    bool opt_tstamp;
    bool opt_tid;
    bool opt_srcdir;
    bool opt_srcfile;
    bool opt_srcline;
    bool opt_func;
    bool opt_why;
    bool opt_newline;

    // If opt_flood is set *AND* DIAG_FLOOD_ENABLE is defined, then
    // the first BOOL argument to diag macros is shortcircuited
    // to true.
    bool opt_flood  = false;

    osvstream os;
    // The intermediate_stream is where DIAG actually writes.  It can be
    // manipulated, e.g.,
    //   the_diag().diag_intermediate_stream.precision(17)
    // but it shouldn't be written to directly or flushed.
    std::ostream& diag_intermediate_stream;
    log_channel logchan;

    // private - do not call or modify
    //
    // We can't make them truly private because we use them in our macro
    // expansions, but users *MUST NOT* call or modify them.  The names
    // start with an _, which should remind you that they're special.
    std::recursive_mutex _diag_mtx;
    std::ostream& _diag_before(const char* k, const char *file, int line, const char *func){
        if(opt_tstamp){
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
            struct tm now_tm;
            struct tm *now_tmp = ::localtime_r(&now_timet, &now_tm); // how slow  is localtime?   Do we care?
            if(now_tmp){
                // N.B.  this is actually faster than stringprintf (gcc6, 2017)
                auto oldfill = os.fill('0');
                // E.g., "19:09:51.779321 mount.fs123p7.cpp:862 [readdir] "
                os << std::setw(2)  << now_tm.tm_hour << ':' << std::setw(2) << now_tm.tm_min << ":" << std::setw(2) << now_tm.tm_sec << "." << std::setw(6) << musec << ' ';
                os.fill(oldfill);
            }
        }
        if(opt_tid){
#if defined(__linux__)
            os << '[' << ::syscall(SYS_gettid) << "] ";
#else
            std::ios::fmtflags oldflags(os.flags());
            os << std::hex << '[' << std::this_thread::get_id() << "] ";
            os.flags(oldflags);
#endif
        }
        if(opt_srcfile || opt_srcdir){
            std::string filepart;
            std::string dirpart;
            std::tie(dirpart, filepart) = core123::pathsplit(file);
            if(opt_srcdir)
                os << dirpart << '/';
            if(opt_srcfile)
                os << filepart;
        }
        if(opt_srcline)
            os << ':' << line;
        if(opt_func)
            os << func << "() ";
        if(opt_why)
            os << "[" << k << "] ";
        return os;
    }

    void _diag_after(const char* /*k*/, const char */*file*/, int /*line*/, const char */*func*/){
        if(opt_newline)
            os.put('\n');
        logchan.send(os.sv());
        os.clear();
        os.str({});
    }

    // It's a singleton class.  The only way to construct one is through 'the_diag'
    // which will only ever construct one.
    friend diag_t& the_diag();
private:
    diag_t() :
        diag_intermediate_stream(os),
        logchan("%stderr", 0),
        thenames(new core123::named_ref_space<int>)
    {
        const char *p;
        set_diag_names( (p=::getenv("CORE123_DIAG_NAMES")) ? p : "");
        set_diag_opts( (p=::getenv("CORE123_DIAG_OPTS")) ? p : ""); // calls set_opt_defaults
        set_diag_destination( (p=::getenv("CORE123_DIAG_DESTINATION")) ? p : "%stderr");
    }

    // clear_names:  zero out all the diag_names:
    void clear_names(){
        // can't just say kv.second = 0 because getmap
        // returns a const map.  So we jump through the
        // declare hoop to get something assignable.
        for(auto& kv : thenames->getmap())
            (int&)thenames->declare(kv.first) = 0;
    }

    void set_opt_defaults(){
        opt_tstamp = false;
        opt_tid = false;
        opt_srcdir = false;
        opt_srcfile = false;
        opt_srcline = false;
        opt_func = true;
        opt_why = true;
        opt_newline = false;
        opt_flood = false;
    }

    // thenames is a bare, new'ed pointer?  It leaks!  This is
    // intentional, to avoid the static destructor fiasco when
    // everything gets torn down.
    named_ref_space<int>* thenames;
};

inline diag_t& the_diag(){
    static diag_t _the_diag;
    return _the_diag;
}

inline named_ref<int> diag_name(const std::string& s, int initial_value = 0){ return the_diag().diag_name(s, initial_value); }
inline void set_diag_names(const std::string& s, bool clear_before_set = true){ return the_diag().set_diag_names(s, clear_before_set); }
inline std::string get_diag_names(bool showall=false){ return the_diag().get_diag_names(showall); }

inline void set_diag_destination(const std::string& diagfname, int mode=0666) { the_diag().set_diag_destination(diagfname, mode); } // umask is respected
inline void set_diag_opts(const std::string& optarg, bool restore_defaults_before_set = true) { the_diag().set_diag_opts(optarg, restore_defaults_before_set); }
inline std::string get_diag_opts() { return the_diag().get_diag_opts(); }


}// namespace core123

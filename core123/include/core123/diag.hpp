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

  will be executed in a static initializer during program startup.

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
  Therefore library code should generally call diag__name at
  function scope, and avoid using it at function scope.


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

  The formatting is controlled by a few booleans in the diag namespace:

   diag_opt_tstamp - include a microsecond timestamp in each output record
   diag_opt_tid - include the thread-id of the calling process in each output record
   diag_opt_srcdir - include the directory part of __FILE__ in each output record
   diag_opt_srcfile - include the last component of __FILE__ in each output record
   diag_opt_srcline - include the value of __LINE__ in each output record
   diag_opt_func   - include the value of __func__ in each output record
   diag_opt_why    - include the stringified bool_expr in every output record
   diag_opt_newline - unconditionally add a newline to every output record
   diag_opt_flood - turns on all DIAG/DIAGf macros unconditionally

  opt_func and opt_why are true by default.  All others are
  false by default.

  You can set the opt_ options in your code by assigning directly to the
  variables in the diag namespace.  They can also be set with a string argument:

     set_diag_opts(const std::string& opts, bool reset_before_set=true);

  The string argument is expected to contain colon or
  whitespace-separated names of format options without the
  opt_ prefix e.g. "tstamp:tid:nonewline".  Options can be
  preceded by "no" to explicitly disable an option.  All
  options are reset to their default values before parsing
  the opts string if and only if reset_before_set is true.
  The following code will be executed at static
  initialization time:

      set_diag_destination(::getenv("CORE123_DIAG_OPTS") || "");

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

#include "strutils.hpp"
#include "named_ref.hpp"
#include <string>
#include <iostream>
#include <mutex>

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
/* enables the diag_all boolean shortcircuit.  This probably
   hurts diag performance (when off) by a tiny amount, so it
   is not on by default.  But it is convenient to have the capability
   for testing a program to ensure all diags workm and produce
   meaningful output without having to grep for and turn on all
   diags manually.
*/
#define _diag_flood core123::diag_opt_flood
#else
#define _diag_flood 0
#endif /* DIAG_FLOOD_ENABLE */

#define DIAGloc(BOOL, _file, _line, _func, _expr) do{                   \
        if( __diag_unlikely(_diag_flood || bool(BOOL)) ){               \
            std::lock_guard<std::recursive_mutex> __diag_lg(core123::_diag_mtx); \
            core123::_diag_before( #BOOL , _file, _line, _func) << _expr; \
            core123::_diag_after( #BOOL , _file, _line, _func);        \
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

// "methods" for working with the names in the named_ref_space.
named_ref<int> diag_name(const std::string&, int initial_value = 0);
void set_diag_names(const std::string& s, bool clear_before_set = true);
std::string get_diag_names(bool showall=false); // showall says whether to report zero-valued keys.

// "methods" for working with the destination
void set_diag_destination(const std::string& diagfname, int mode=0666); // umask is respected

// formatting options (with defaults) and methods for setting and
// getting them from/to a colon-separated string:
extern bool diag_opt_tstamp;  // false
extern bool diag_opt_tid;     // false
extern bool diag_opt_srcdir;  // false
extern bool diag_opt_srcfile; // false
extern bool diag_opt_srcline; // false
extern bool diag_opt_func;    // true
extern bool diag_opt_why;     // true
extern bool diag_opt_newline; // false

// If opt_flood is set *AND* DIAG_FLOOD_ENABLE is defined, then
// the first BOOL argument to diag macros is shortcircuited
// to true.
extern bool diag_opt_flood;	// false

void set_diag_opts(const std::string&, bool restore_defaults_before_set = true);
std::string get_diag_opts();

// The intermediate_stream is where DIAG actually writes.  It can be
// manipulated, e.g.,
//   core123::diag_intermediate_stream.precision(17)
// but it shouldn't be written to directly or flushed.
extern std::ostream& diag_intermediate_stream;

// private - do not call or modify
//
// We can't make them truly private because we use them in our macro
// expansions, but users *MUST NOT* call or modify them.  The names
// start with an _, which should remind you that they're special.
extern std::ostream& _diag_before(const char* k, const char *file, int line, const char *func);
extern void _diag_after(const char* k, const char *file, int line, const char *func);
extern std::recursive_mutex _diag_mtx;

}// namespace core123

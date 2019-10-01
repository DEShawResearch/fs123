// Small option parsing utility class.  Only intended for name=value
// format, which can directly come from argv via --name=value, or
// environment variables {SOMEPREFIX}{NAME}=value, or from the caller.

// Options are decalared with:
//
//   option_parser p;
//   p.add_option("name", "description", "default_value", callback)
//   ...
//
// Where 'callback' is any 'Callable' with a signature like:
//
//    void callable(const std::string& newvalue, const option& before);
//
// The callable can do whatever it likes with the new value and the
// previous state of the option (see option's api below).  E.g., it
// can convert the new value to a type and/or write it to an external
// location.  In fact, the 'opt_setter' function returns a callable
// that does exactly that.  E.g.,
//
//   bool verbosity;
//   int nthreads;
//   p.add_option("verbose", "chattiness", "0", opt_setter(verbosity));
//   p.add_option("nthreads", "how many threads", "4", opt_setter(nthreads));
//
// When the '--verbose=true' option is parsed, the functor returned by
// opt_setter(verbosity) will assign true to its argument, verbosity.
// Note that opt_setter stores a reference to its argument, so its
// argument should generally be a long-lived object.
//
// Also note that the callback is always invoked at least once, by
// add_option itself, with newvalue equal to the specified default
// value.
//
// A rudimentary, but passable usage string is produced by:
//   std::cerr << "Options:\n" <<  p.helptext();
//
// Options are set in several ways:
//
// - from the command line:
//     p.setopts_from_argv(int argc, const char** argv, size_t startindex=1);
// - from the environment:
//     p.setopts_from_env("MYPROG_");
// - from a stream:
//     p.setopts_from_istream(ifstream("foo.config"));
// - explicitly:
//     p.set("name", "value");
//   or
//     auto& to_opt = p.add_option("timeout", "how long to wait", "10", opt_setter(timeout));
//     to_opt.set("99");
//
// In all cases, option names are case-insensitive and hyphens and underscores
// in option names are ignored.  So,
//    --verbose=1
//    --ver-bose=1
//    --VERBOSE=1
//    --VERBO_se=1
//    env MYPROG_VERBOSE=1 ...
//    env MYPROG_verbose=1 ...
// are all equivalent:  they will all invoke the callback associated with the "verbose" option.

// Advanced usage:  (subject to change!):
//
// The option class provides access to option details:
// The p.add_option method returns an 'option' class.
//   option opt = p.add_option(...);
// The option class has the following methods:
//   opt.get_name()  - returns the name by which this option may be set
//   opt.get_value() - returns the current string value of the option
//   opt.get_default() - returns the default value of the option
//   opt.get_desc()  - returns the option's description
//   opt.set(const std::string&) - sets the value and calls the callback
//
// The option_parser class exposes the details of all options via
// the get_map method:
//
//   class option_parser{
//     public:
//       typedef std::map<std::string, option> OptMap;
//       const option_parser::OptMap& p.get_map() const;
//       ...
//   };
//
// Simplicity is a virtue: Every option is initialized to an
// explicitly specified default value.  Every option has a
// well-defined value at all times.  Every option specification is of
// the form --name=value.  There are no plain --name option
// specifications.  There's no such thing as a "not yet set" option.
// There is no such thing as 'unset'-ing an option.
// 
// It's not that hard to make different decisions about all of the
// above (hint: use pointers).  But is it worth the added complexity?

#pragma once
#include <string>
#include <map>
#include <cstring>
#include <iostream>
#include <fstream>
#include <core123/svto.hpp>
#include <core123/throwutils.hpp>

namespace core123{

struct option;
namespace detail{
template<typename T>
class _setter{
    T& v;
public:
    void operator()(const std::string& newv, const option&){ v = svto<T>(newv); }
    _setter(T& v_) : v(v_){}
};

template<>
void _setter<std::string>::operator()(const std::string& newv, const option&){ v = newv; }
} // namespace detail

template<typename T>
detail::_setter<T>
opt_setter(T& v){ return detail::_setter<T>(v); }

struct option{
    std::string name;    // identical to the key in optmap.
    std::string desc;    // description for help text
    std::string valstr;  // current value, initialized to dflt
    std::string dflt;    // default value
    std::function<void(const std::string&, const option&)> callback;
    friend class option_parser;
    // N.B.  the only way to construct an option is through option_parser::add_option
    option(const std::string& name_, const std::string& dflt_, const std::string& desc_, std::function<void(const std::string&, const option&)> cb_):
        name(name_), desc(desc_), dflt(dflt_), callback(cb_)
    { set(dflt); }
public:
    std::string get_name() const { return name; }
    std::string get_value() const { return valstr; }
    std::string get_default() const { return dflt; }
    std::string get_desc() const { return desc; }
    void set(const std::string& newval){
        callback(newval, *this);
        valstr = newval;
    }
};

class option_parser {
public:
    typedef std::map<std::string, option> OptMap;
private:
    // Note that add_option returns a reference to the newly added
    // option *in* the optmap_.  Therefore, nothing may ever be
    // removed from optmap_!
    OptMap optmap_;
    // When parsing command-line options, we ignore hyphens, underscores
    // and case.  Canonicalize is called before keys are inserted
    // or looked up in optmap_.
    std::string canonicalize(const std::string& word){
        std::string ret;
        for(auto letter : word){
            if(letter == '-' || letter == '_')
                continue;
            ret.append(1, std::tolower(letter));
        }
        return ret;
    }
public:
    // creates and returns a new option.
    option& add_option(const std::string& name, const std::string& dflt, const std::string& desc, std::function<void(const std::string&, const option&)> cb){
        // N.B.  If the name already exists, it is *NOT* overwritten and it is *NOT* an error.
        return optmap_.emplace(std::piecewise_construct, std::forward_as_tuple(canonicalize(name)), std::forward_as_tuple(name, dflt, desc, cb)).first->second;
    }

    // set one option, by name, to val, and call the option's callback.
    void set(const std::string &name, const std::string &val){
        // throws if name is not a known option.
        optmap_.at(canonicalize(name)).set(val);
    }
       
    // utility function to set an individual option by parsing name=value
    // out from nvp
    void set(const char *nvp){
        auto p = strchr(nvp, '=');
        if (p == nullptr)
            throw std::runtime_error(std::string("option missing = sign, need name=value, not ") + nvp);
        std::string name{nvp, static_cast<size_t>(p - nvp)};
        set(name, p+1);
    }

    // How much visibility should we offer into internals?  With a
    // const reference to the optmap, a determined caller can
    // enumerate the current option settings or generate its
    // own helptxt.  Sufficient?
    const OptMap& get_map() const { return optmap_; }

    // parses any --foo=bar from argv[startindex] onwards after stripping off leading --
    // stops at -- (gobbling  it) or - non-hyphen.  foo must be an option name
    // that was previously add_option()-ed.
    // N.B. - only *weakly* exception-safe.  *This will have been modified in
    // unpredictable ways if the operation throws.
    int setopts_from_argv(int argc, const char **argv, size_t startindex = 1){
        int optind;
        for (optind = startindex; optind < argc; optind++) {
            auto cp = argv[optind];
            if (cp[0] == '-') {
                // might be --name=value
                if (cp[1] == '-') {
                    // --name=value or --
                    if (cp[2] == '\0') {
                        // --, gobble it and return
                        optind++;
                        break;
                    }
                    set(&cp[2]);
                } else if (cp[1] == '@') {
                    if (cp[2] == '\0') {
                        throw std::runtime_error(std::string("need filename containing options after ") + cp);
                    } else {
                        std::ifstream inf(&cp[2]);
                        setopts_from_istream(inf);
                    }
                } else if (cp[1] != '\0') {
                    throw std::runtime_error(std::string("single -option not supported, need --option=value, not ") + cp);
                } else {
                    // bare - might mean stdin, we return on it as if non-option
                    break;
                }
            } else {
                // no leading -, so first non-option, return
                break;
            }
        }
        return optind;
    }        

    // sees if any environment variable names of the form opt_env_prefix
    // concatenated before the uppercase option name exist, and if so, set
    // that option to the corresp value (for all names provided in opts to init)
    // N.B. - only *weakly* exception-safe.  *This will have been modified in
    // unpredictable ways if the operation throws.
    void setopts_from_env(const char *opt_env_prefix){
        std::string pfx(opt_env_prefix);
        for (const auto& o : optmap_) {
            std::string ename(opt_env_prefix);
            for (const char *s = o.first.c_str(); *s; s++) {
                unsigned char c = *s;
                ename += toupper(c);
            }
            auto ecp = getenv(ename.c_str());
            if (ecp) set(o.first, ecp);
        }
    }

    // read options from the specified file
    // N.B. - only *weakly* exception-safe.  *This will have been modified in
    // unpredictable ways if the operation throws.
    void setopts_from_istream(std::istream& inpf){
        for (std::string line; getline(inpf, line);) {
            std::string s = strip(line); // remove leading and trailing(?) whitespace
            if(startswith(s, "#") || s.empty())
                continue;
            if(startswith(s, "--"))
                s = s.substr(2);
            set(s.c_str());
        }
    }        
    
    // returns help text derived from names, defaults and descriptions.
    std::string helptext(size_t indent = 4) const{
        std::string ret;
        for (const auto& o : optmap_) {
            ret.append(indent, ' ');
            ret.append(o.second.name); // not o.first,  which is canonicalized
            ret.append(1, '=');
            ret.append(" (default ");
            ret.append(o.second.dflt);
            ret.append(" ) : ");
            ret.append(o.second.desc);
            ret.append(1, '\n');
        }
        return ret;
    }
};

} // namespace core123

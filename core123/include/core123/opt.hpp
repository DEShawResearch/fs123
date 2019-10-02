// Small option parsing utility class.  Only intended for name=value
// format, which can directly come from argv via --name=value, or
// environment variables {SOMEPREFIX}{NAME}=value, or from the caller.
//
// An option_parser is declared with:
//
//   option_parser p("myprog - a program for doing my stuff\nUsage:  myprog [options] foo bar\nOptions:\n");
//   ...
//
// The optional string argument to the option_parser constructor will
// appear at the beginning of helptext(), and the output of the --help
// option.
//
// Individual options are declared with:
//
//   p.add_option("name", "description", default_value, callback)
//
// where callback has a signature like:
//
//    void callable(const std::string& newvalue, const option& before);
//
// Such options are set by a command line argument of the form:
//
//    --name=value
//
// The callable can do whatever it likes with the new value and the
// previous state of the option (see option's api below).  E.g., it
// can convert the new value to a type and/or write it to an external
// location.  In fact, the 'opt_setter' function returns a callable
// that does exactly that.  E.g.,
//
//    int nthreads;
//    p.add_option("nthreads", "number of threads", 4, opt_setter(nthreads));
//
// When the '--nthreads=4' option is parsed, the functor returned by
// opt_setter(nthreads) will assign 4 to its argument, nthreads.  Note
// that the opt_setter stores a reference to its argument, so its
// argument should generally be a long-lived objects.
//
// Options that do not require a value are declared without a default and
// with a callback with a different signature:
//
//   p.add_option("name", "description", callback);
//
// In which case, the callback's signature is:
//
//     void callable(const option& before);
//
// The 'opt_true_setter' and 'opt_false_setter' callbacks are particularly
// useful for value-less options that set a boolean:
//
//   bool verbose
//   p.add_option("verbose", "chattiness", opt_true_setter(verbose));
//
// When the '--verbose' option is parsed, the functor returned by
// opt_true_setter(verbosity) will assign true to its argument,
// verbosity.
//
// There are two automatically declared options:
//
//   --help - invokes a callback that inserts helptext() in std::cerr
//   --flagfile=FILENAME - invokes a callback that calls:
//           setopts_from_istream(ifstream(FILENAME))
//
// To override the pre-declared options (or any existing option), use del_option before
// calling add_option:
//
//   bool help;
//   p.del_option("help");
//   p.add_option("help", "send help text to an undisclosed location", opt_true_setter(help));
//
// A rudimentary, but passable usage string is produced by:
//   std::cerr << p.helptext();
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
//     p.set("verbose");  // a no-value option
//
// In all cases, option names are case-insensitive and hyphens and underscores
// in option names are ignored.  So,
//    --max-open-files=1024
//    --max_open_files=1024
//    --maxopenfiles=1024
//    --MaxOpenFiles=1024
//    --max_OpENFil-es=1024
//    env MYPROG_MAX_OPEN_FILES=1024 ...
//    env MYPROG_max_open_files=1024 ...
// are all equivalent:  they will all invoke the callback associated with the "verbose" option.
//
// The option_parser methods generally throw option_parser exceptions
// (possibly with other exceptions nested inside) if they encounter
// any unexpected conditions.
//
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

class opt_true_setter{
    bool& v;
public:
    void operator()(const option&){ v = true; }
    opt_true_setter(bool& v_) : v(v_){}
};

class opt_false_setter{
    bool& v;
public:
    void operator()(const option&){ v = false; }
    opt_false_setter(bool& v_) : v(v_){}
};

struct option_error : public std::runtime_error{
    explicit option_error(const std::string& what_arg) : std::runtime_error(what_arg){}
    explicit option_error(const char* what_arg) : std::runtime_error(what_arg){}
    ~option_error() = default;
};

struct option{
    std::string name;    // identical to the key in optmap.
    std::string desc;    // description for help text
    std::string valstr;  // current value, initialized to dflt
    std::string dflt;    // default value
    // only one of value_callback and novalue_callback will be valid
    std::function<void(const std::string&, const option&)> value_callback;
    std::function<void(const option&)> novalue_callback;
    friend class option_parser;
    // N.B.  the only way to construct an option is through option_parser::add_option
    option(const std::string& name_, const std::string& dflt_, const std::string& desc_, std::function<void(const std::string&, const option&)> cb_):
        name(name_), desc(desc_), dflt(dflt_), value_callback(cb_)
    { set(dflt); }
    option(const std::string& name_, const std::string& desc_, std::function<void(const option&)> cb_):
        name(name_), desc(desc_), novalue_callback(cb_)
    { }
    bool value_required() const { return bool(value_callback); }
public:
    std::string get_name() const { return name; }
    std::string get_value() const { return valstr; }
    std::string get_default() const { return dflt; }
    std::string get_desc() const { return desc; }
    void set(){
        novalue_callback(*this);
    }
    void set(const std::string& newval){
        value_callback(newval, *this);
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
    std::string description;
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

    // utility function to set an individual option by parsing name=value
    // or just name from nvp
    void setarg(const char *nvp){
        auto p = strchr(nvp, '=');
        if (p == nullptr){
            set(nvp);
        }else{
            std::string name{nvp, static_cast<size_t>(p - nvp)};
            set(name, p+1);
        }
    }

public:
    option_parser(const std::string& desc = "Options:\n") : description(desc) {
        if(!endswith(description, "\n") && !description.empty())
            description += "\n";
        add_option("help", "send helptext to stderr", [this](const option&){ std::cerr << helptext(); });
        add_option("flagfile", "read flags from the named file", "",
                   [this](const std::string& fname, const option&){
                       if(fname.empty())
                           return;
                       std::ifstream ifs(fname);
                       setopts_from_istream(ifs);
                   });
    }
    // creates and returns a new option.
    option& add_option(const std::string& name, const std::string& dflt, const std::string& desc, std::function<void(const std::string&, const option&)> cb) try {
        auto ibpair = optmap_.emplace(std::piecewise_construct, std::forward_as_tuple(canonicalize(name)), std::forward_as_tuple(name, dflt, desc, cb));
        if(!ibpair.second)
            throw option_error("opt_parser::add_option(" + name + ") already exists.");
        return ibpair.first->second;
    }
    catch(option_error& oe){throw;}
    catch(std::exception& e){std::throw_with_nested(option_error("option_error::" + strfunargs(__func__, name, dflt, desc, &cb)));}

    option& add_option(const std::string& name, const std::string& desc, std::function<void(const option&)> cb) try{

        auto ibpair = optmap_.emplace(std::piecewise_construct, std::forward_as_tuple(canonicalize(name)), std::forward_as_tuple(name, desc, cb));
        if(!ibpair.second)
            throw option_error("opt_parser::add_option(" + name + ") already exists.");
        return ibpair.first->second;
    }
    catch(option_error&){throw;}
    catch(std::exception& e){std::throw_with_nested(option_error("option_error::" + strfunargs(__func__, name, desc, &cb)));}
    
    void del_option(const std::string& name)try{
        optmap_.erase(name);
    }
    catch(std::exception& e){std::throw_with_nested(option_error("option_error::" + strfunargs(__func__, name)));}

    // set one option, by name, to val, and call the option's callback.
    void set(const std::string &name, const std::string &val)try{
        // .at throws if name is not a known option.
        // .set throws if the name was not declared to accept a value
        optmap_.at(canonicalize(name)).set(val);
    }
    catch(std::exception& e){std::throw_with_nested(option_error("option_error::" + strfunargs(__func__, name, val)));}
       
    // call the novalue callback associated with name
    void set(const std::string &name)try{
        // .at throws if name is not a known option.
        // .set throws if the name was not declared as a no-value option
        optmap_.at(canonicalize(name)).set();
    }
    catch(std::exception& e){std::throw_with_nested(option_error("option_error::" + strfunargs(__func__, name)));}
       
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
    int setopts_from_argv(int argc, const char **argv, size_t startindex = 1)try{
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
                    setarg(&cp[2]);
                } else if (cp[1] != '\0') {
                    throw option_error(std::string("single -option not supported, need --option=value, not ") + cp);
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
    catch(option_error&){ throw; }
    catch(std::exception& e){std::throw_with_nested(option_error("option_error::" + strfunargs(__func__, argc, argv, startindex)));}

    // sees if any environment variable names of the form opt_env_prefix
    // concatenated before the uppercase option name exist, and if so, set
    // that option to the corresp value (for all names provided in opts to init)
    // N.B. - only *weakly* exception-safe.  *This will have been modified in
    // unpredictable ways if the operation throws.
    void setopts_from_env(const char *opt_env_prefix)try{
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
    catch(option_error&){ throw; }
    catch(std::exception& e){std::throw_with_nested(option_error("option_error::" + strfunargs(__func__, opt_env_prefix)));}

    // read options from the specified file
    // N.B. - only *weakly* exception-safe.  *This will have been modified in
    // unpredictable ways if the operation throws.
    void setopts_from_istream(std::istream& inpf)try{
        for (std::string line; getline(inpf, line);) {
            std::string s = strip(line); // remove leading and trailing(?) whitespace
            if(startswith(s, "#") || s.empty())
                continue;
            if(startswith(s, "--"))
                s = s.substr(2);
            setarg(s.c_str());
        }
    }        
    catch(option_error&){ throw; }
    catch(std::exception& e){std::throw_with_nested(option_error("option_error::" + strfunargs(__func__, &inpf)));}
    
    // returns help text derived from names, defaults and descriptions.
    std::string helptext(size_t indent = 4) const try{
        std::string ret = description;
        for (const auto& o : optmap_) {
            const option& opt = o.second;
            ret.append(indent, ' ');
            ret.append(opt.name); // not o.first,  which is canonicalized
            if(opt.value_required()){
                ret.append(1, '=');
                // N.B.  callback is valid only if and only if the option was created with a default.
                ret.append(" (default ");
                ret.append(opt.dflt);
                ret.append(" )");
            }
            ret.append(" : ");
            ret.append(opt.desc);
            ret.append(1, '\n');
        }
        return ret;
    }
    catch(option_error&){ throw; }
    catch(std::exception& e){std::throw_with_nested(option_error("option_error::" + strfunargs(__func__, indent)));}
};

} // namespace core123

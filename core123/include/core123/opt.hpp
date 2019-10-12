// Small option parsing utility class.  Only intended for name=value
// format, which can directly come from argv via --name=value, or
// environment variables {SOMEPREFIX}{NAME}=value, or from the caller.
//
// An option_parser is declared with:
//
//   option_parser p;
//   ...
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
// - from a range:
//     p.setopts_from_range(ITER b, ITER e);
//     N.B.  the iterator's value-type must be *convertible* to std::string.
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
// When reading options from a stream, the stream is read one line at
// a time.  Whitespace is trimmed from the beginning and end of each
// line, and if the result is either empty or starts with a '#', the
// line is ignored.  Otherwise, the line must match the regex:
//
//    std::regex re("(--)?([-_[:alnum:]]+)\\s*(=?)\\s*(.*)");
//
// I.e., the line must start with an optional leading "--", followed
// by the option name (one or more hyphens, underscores or
// alphanumerics), followed by an optional "=" (surrounded by optional
// whitespace), followed by the value (everything to the end of the
// line).
// 
// If there is no "=", *and* if there is only whitespace after the
// name, the named option must not be value_required().
// Conversely, if there is an "=" *or* if there is non-whitespace
// after the name, the option must not be value_required().  For
// example:
//
//     --foo             // no-value
//     --foo=bar         // with-value("bar")
//     --foo =  bar      // with-value("bar")
//     --foo== bar       // with-value("= bar")
//     --foo bar         // with-value("bar")
//     --foo     bar     // with-value("bar")
//     --foo bar=baz     // with-value("bar=baz")
//     --foo=bar=baz     // with-value("bar=baz")
//     #--foo            // comment
//     foo               // no-value
//     foo=bar           // with-value("bar")
//     foo    bar        // with-value("bar")
//     #foo              // comment
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
//   opt.value_required() - returns true if the option requires a value and has a default.
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
#include <core123/svto.hpp>
#include <core123/throwutils.hpp>
#include <string>
#include <map>
#include <cstring>
#include <iostream>
#include <fstream>
#include <regex>
#include <stdexcept>

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
public:
    bool value_required() const { return bool(value_callback); }
    std::string get_name() const { return name; }
    std::string get_value() const {
        if(!value_required())
            throw option_error("option::get_value:  option --" + name + "does not support values");
        return valstr;
    }
    std::string get_default() const {
        if(!value_required())
            throw option_error("option::get_default_value:  option --" + name + "does not support values");
        return dflt;
    }
    std::string get_desc() const { return desc; }
    void set(){
        if(value_required())
            throw option_error("option::set:  option --" + name + " requires a value");
        novalue_callback(*this);
    }
    void set(const std::string& newval){
        if(!value_required())
            throw option_error("option::set:  option --" + name + " does not support values");
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

    option& at(const std::string& k) try {
        return optmap_.at(canonicalize(k));
    }catch(std::out_of_range&){
        throw option_error("option_parser:  unknown option: " + k);
    }

    auto find(const std::string& k){
        return optmap_.find(canonicalize(k));
    }

public:
    option_parser(){
        static int flagfile_depth = 0;
        add_option("flagfile", "", "read flags from the named file",
                   [this](const std::string& fname, const option&){
                       if(flagfile_depth++ > 10)
                           throw option_error("flagfile recursion depth exceeds limit (10) processing:" + fname);
                       if(fname.empty()) // don't forget, we get called with the default!
                           return;
                       std::ifstream ifs(fname);
                       setopts_from_istream(ifs);
                       if(!ifs && !ifs.eof())
                           throw option_error("error reading from --flagfile=" + fname);
                       flagfile_depth--;
                   });
    }
    // creates and returns a new option.
    option& add_option(const std::string& name, const std::string& dflt, const std::string& desc, std::function<void(const std::string&, const option&)> cb) try {
        auto ibpair = optmap_.emplace(std::piecewise_construct, std::forward_as_tuple(canonicalize(name)), std::forward_as_tuple(name, dflt, desc, cb));
        if(!ibpair.second)
            throw option_error("opt_parser::add_option(" + name + ") already exists.");
        return ibpair.first->second;
    }
    catch(option_error&){throw;}
    catch(std::exception&){std::throw_with_nested(option_error("option_error::" + strfunargs(__func__, name, dflt, desc, &cb)));}

    option& add_option(const std::string& name, const std::string& desc, std::function<void(const option&)> cb) try{

        auto ibpair = optmap_.emplace(std::piecewise_construct, std::forward_as_tuple(canonicalize(name)), std::forward_as_tuple(name, desc, cb));
        if(!ibpair.second)
            throw option_error("opt_parser::add_option(" + name + ") already exists.");
        return ibpair.first->second;
    }
    catch(option_error&){throw;}
    catch(std::exception&){std::throw_with_nested(option_error("option_error::" + strfunargs(__func__, name, desc, &cb)));}
    
    void del_option(const std::string& name)try{
        optmap_.erase(name);
    }
    catch(std::exception&){std::throw_with_nested(option_error("option_error::" + strfunargs(__func__, name)));}

    // set one option, by name, to val, and call the option's callback.
    void set(const std::string &name, const std::string &val)try{
        // .at throws if name is not a known option.
        // .set throws if the name was not declared to accept a value
        at(name).set(val);
    }
    catch(std::exception&){std::throw_with_nested(option_error("option_error::" + strfunargs(__func__, name, val)));}
       
    // call the novalue callback associated with name
    void set(const std::string &name)try{
        // .at throws if name is not a known option.
        // .set throws if the name was not declared as a no-value option
        at(name).set();
    }
    catch(std::exception&){std::throw_with_nested(option_error("option_error::" + strfunargs(__func__, name)));}
       
    // How much visibility should we offer into internals?  With a
    // const reference to the optmap, a determined caller can
    // enumerate the current option settings or generate its
    // own helptxt.  Sufficient?
    const OptMap& get_map() const { return optmap_; }

    // parses any --foo=bar from argv[startindex] onwards.
    // Stops at -- (gobbling it).  An option_error is thrown if foo
    // was not previously add_option()-ed.  N.B. - only *weakly*
    // exception-safe.  *This will have been modified in unpredictable
    // ways if the operation throws.
    std::vector<std::string>
    setopts_from_argv(int argc, char *argv[], int startindex = 1)try{
        return setopts_from_range(argv+startindex, argv+argc);
    }
    catch(option_error&){ throw; }
    catch(std::exception&){std::throw_with_nested(option_error("option_error::" + strfunargs(__func__, argc, argv, startindex)));}
    
    template <typename ITER>
    std::vector<std::string>
    setopts_from_range(ITER b, ITER e)try{
        std::vector<std::string> leftover;
        for (auto i=b ; i!=e; ++i){
            std::string cp = std::string(*i);
            if(!startswith(cp, "--")){
                leftover.push_back(cp);
                continue;
            }
            if(cp == "--"){
                // Stop when we see --.  Anything left gets pushed
                // onto the leftover vector.
                for( ++i ; i!=e; ++i)
                    leftover.push_back(std::string(*i));
                break;
            }
            auto eqpos = cp.find("=", 2);
            if(eqpos == std::string::npos){
                // No '='.  This might be a no-value argument, or a
                // with-value argument that consumes the next argv.
                auto optiter = find(cp);
                if(optiter == optmap_.end()){
                    leftover.push_back(cp);
                    continue;
                }
                auto& opt = optiter->second;
                if(opt.value_required()){
                    if(++i == e)
                        throw option_error("Missing argument for option:" + cp);
                    std::string arg = std::string(*i);
                    try{
                        opt.set(arg);
                    }catch(std::exception& ){
                        std::throw_with_nested(option_error(std::string(__func__) + ": error while processing " + cp + " " + arg));
                    }
                }else{
                    try{
                        opt.set();
                    }catch(std::exception& ){
                        std::throw_with_nested(option_error(std::string(__func__) + ": error while processing " + cp));
                    }
                }
            }else{
                // --name=something
                auto optiter = find(cp.substr(2, eqpos-2));
                if(optiter == optmap_.end()){
                    leftover.push_back(cp);
                    continue;
                }
                auto& opt = optiter->second;
                try{
                    opt.set(cp.substr(eqpos+1));
                }catch(std::exception& ){
                    std::throw_with_nested(option_error(std::string(__func__) + ": error while processing " + cp));
                }
            }
        }
        return leftover;
    }        
    catch(option_error&){ throw; }
    catch(std::exception&){std::throw_with_nested(option_error("option_error::" + strfunargs(__func__, &*b, &*e)));}

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
    catch(std::exception&){std::throw_with_nested(option_error("option_error::" + strfunargs(__func__, opt_env_prefix)));}

    // read options from the specified istream
    // N.B. - only *weakly* exception-safe.  *This will have been modified in
    // unpredictable ways if the operation throws.
    void setopts_from_istream(std::istream& inpf)try{
        // N.B. - reading to the end of inpf this way may (usually!)
        // set inpf's failbit.
        for (std::string line; getline(inpf, line);) {
            std::string s = strip(line); // remove leading and trailing whitespace
            if(startswith(s, "#") || s.empty())
                continue;
            if(startswith(s, "--"))
                s = s.substr(2);
            static std::regex re("(--)?([-_[:alnum:]]+)\\s*(=?)\\s*(.*)");
            std::smatch mr;
            if(!std::regex_match(s, mr, re))
                throw option_error("setopts_from_istream: failed to parse line: " +line);
            if(mr.size() != 5)
                throw std::logic_error("Uh oh. We're very confused about regex_match");
            auto name = mr.str(2);
            auto equals = mr.str(3);
            auto rhs = mr.str(4);
            if(!equals.empty() || !rhs.empty())
                set(name, rhs);
            else
                set(name);
        }
    }        
    catch(option_error&){ throw; }
    catch(std::exception&){std::throw_with_nested(option_error("option_error::" + strfunargs(__func__, &inpf)));}
    
    // returns help text derived from names, defaults and descriptions.
    std::string helptext(size_t indent = 4) const try{
        std::string ret;
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
    catch(std::exception&){std::throw_with_nested(option_error("option_error::" + strfunargs(__func__, indent)));}
};

} // namespace core123

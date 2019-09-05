// Small option parsing utility class.  Only intended for
// name=value format, which can directly come from argv via --name=value,
// or environment variables {SOMEPREFIX}{NAME}=value, or as (name,value)
// strings or just name=value.
// Mark Moraes, D. E. Shaw Research
#pragma once
#include <string>
#include <cstring>
#include <utility>
#include <vector>
#include <map>
#include <initializer_list>

enum OptionType {
#   define OPT_TEMPLATE(TYPE, DESC, ENUM) ENUM,
#   include "detail/opt_template.hpp"
};

typedef struct {
    std::string oname;	  // name of option, e.g. "verbose"
    OptionType otype;
    std::string odefault; // stringified default value, to be used if getenv() returns nullptr
    void *ovalp;	  // MUST point to appropriate size storage: uint32_t if otype == u32,
                          // uint64_t if otype == u64, std::string if otype == str, double if dbl
                          // int32_t if otype == i32, int64_t if otype == i64
    std::string odesc;    // description for help text
    std::string valstr;   // stringified set value
} Option;

class OptionParser {
private:
    typedef std::map<std::string, Option> OptMap;
    OptMap optmap_;
    // sets the value of o to string val (after converting suitably for numbers)
    void set(Option &o, const std::string &val);
public:
    OptionParser() { } ;
    OptionParser(const OptionParser&) = delete;
    OptionParser& operator = (const OptionParser&) = delete;

    // adds all option definitions in opts initializer list to the OptionParser
    // internal map, invoking set for each one to initialize the variable
    // pointed to by ovalp
    void add_options(const std::initializer_list<Option> &opts);

    // passthrough access functions for the internal optmap e.g.
    // to make this class conveniently iterable via range-for, just like a map
    OptMap::const_iterator begin() const { return optmap_.cbegin(); }
    OptMap::const_iterator end() const { return optmap_.cend(); }
    void clear() { optmap_.clear(); }
    const Option& at(const std::string &key) const { return optmap_.at(key); }

    // utility function to set an individual option by name to value (after conversion)
    void set(const std::string &name, const std::string &val);

    // utility function to set an individual option by parsing name=value
    // out from nvp
    void set(const char *nvp);

    // parses any --foo=bar from argv[1] onwards after stripping off leading --
    // stops at -- (gobbling  it) or - non-hyphen.  foo must be an option name
    // that was part of the opts list to init()
    int setopts_from_argv(int argc, const char **argv, size_t startindex = 1);

    // sees if any environment variable names of the form opt_env_prefix
    // concatenated before the uppercase option name exist, and if so, set
    // that option to the corresp value (for all names provided in opts to init)
    void setopts_from_env(const char *opt_env_prefix);

    // read options from the specified file
    void setopts_from_file(const char *filename);
    
    // appends help text derived from oname, otype, odefault, odesc to *retp
    // returns retp->c_str()
    const char *helptext(std::string *retp, size_t indent = 4) const;
};

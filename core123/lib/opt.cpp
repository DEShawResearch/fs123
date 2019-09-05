// simple option setting from environment variables
// TODO: accept vector of X=Y options as well as environ?
#include <string>
#include <fstream>
#include <sstream>
#include <cinttypes>
#include <cctype>
#include <system_error>
#include <cstdio>
#include <core123/diag.hpp>
#include <core123/svto.hpp>
#include <core123/strutils.hpp>
#include <core123/throwutils.hpp>
#include <core123/opt.hpp>

using namespace std;
using namespace core123;

class Usage : public string {
public:
    Usage(const char *s1, const char *s2, const OptionParser *optp, const Option *op = nullptr) {
	append(s1);
	append(s2);
	if (op) {
	    append(" for option ");
	    append(op->oname);
	}
	append("\n");
	optp->helptext(this);
    }
};

// parsearg is a helper function for OptionParser::set to make it easier
// to use the macro-expansion trick uniformly on all types
template <typename T> T parsearg(const string &s) {
    return svto<T>(s);
}
// svTo does not like empty strings and we do not need the conversion
// capabilities or white-space skipping of svTo for strings, so
// specialize parsearg.
template <> string parsearg(const string &s) {
    return s;
}

void OptionParser::set(Option &o, const string &val) {
    static auto _opt = diag_name("opt");

    bool parsed = false;
    o.valstr = val;

#   define OPT_TEMPLATE(TYPE, DESC, ENUM) \
    if (o.otype == OptionType::ENUM) try { \
	auto v = parsearg<TYPE>(val); \
	DIAG(_opt > 1, "set option \"" << o.oname << "\"=\"" << o.valstr << "\" =" << v); \
	*(TYPE *)o.ovalp = v; \
	parsed = true; \
    } catch (invalid_argument& e) { \
	throw_with_nested(runtime_error(Usage("Error parsing argument ", "", this, &o))); \
    }
#   include "core123/detail/opt_template.hpp"
    if (!parsed) {
	throw runtime_error(Usage("Unknown option type, should not happen! ",
				       to_string(o.otype).c_str(), this, &o));
    }
}

void OptionParser::set(const string &name, const string &val) {
    auto p = optmap_.find(name);
    if (p == optmap_.end()) {
	throw runtime_error(Usage("Invalid option name ", name.c_str(), this));
    }
    set(p->second, val);
}

void OptionParser::set(const char *nvp) {
    if ((nvp[0] == '?' && nvp[1] == '\0') || strcasecmp(nvp, "help") == 0)
	throw runtime_error(Usage("OptionParser usage ", nvp, this));
    auto p = strchr(nvp, '=');
    if (p == nullptr)
	throw runtime_error(Usage("option missing = sign, need name=value, not ", nvp, this));
    string name{nvp, static_cast<size_t>(p - nvp)};
    set(name, p+1);
}

void OptionParser::add_options(const initializer_list<Option> &opts) {
    for (const auto& o : opts) {
	auto owpair = optmap_.emplace(o.oname, o);

	const char *cp = o.odefault.c_str();
	set(owpair.first->second, cp);
    }
}

// OptionTypeNames is indexed by OptionType enum
static const char *OptionTypeNames[] = {
#   define OPT_TEMPLATE(TYPE, DESC, ENUM) DESC,
#   include "core123/detail/opt_template.hpp"
};

const char *OptionParser::helptext(string *retp, size_t indent) const {
    for (const auto& o : optmap_) {
	retp->append(indent, ' ');
	retp->append(o.second.oname);
	retp->append(1, '=');
	retp->append(OptionTypeNames[o.second.otype]);
	retp->append(" (default ");
	if (o.second.otype == OptionType::ostr) retp->append(1, '"');
	retp->append(o.second.odefault);
	if (o.second.otype == OptionType::ostr) retp->append(1, '"');
	retp->append(" ) : ");
	retp->append(o.second.odesc);
	retp->append(1, '\n');
    }
    return retp->c_str();
}

int OptionParser::setopts_from_argv(int argc, const char **argv,
				    size_t startindex) {
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
		    throw runtime_error(Usage("need filename containing options after ", cp, this));
		} else {
		    setopts_from_file(&cp[2]);
		}
	    } else if (cp[1] != '\0') {
		throw runtime_error(Usage("single -option not supported, need --option=value, not ", cp, this));
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

void OptionParser::setopts_from_env(const char *opt_env_prefix) {
    static auto _opt = diag_name("opt");

    string pfx(opt_env_prefix);
    for (const auto& o : optmap_) {
	string ename(opt_env_prefix);
	for (const char *s = o.second.oname.c_str(); *s; s++) {
	    unsigned char c = *s;
	    ename += toupper(c);
	}
	auto ecp = getenv(ename.c_str());
	DIAGf(_opt, "getenv(\"%s\") = \"%s\"", ename.c_str(), ecp ? ecp : "(nullptr)");
	if (ecp) set(o.first, ecp);
    }
}

void OptionParser::setopts_from_file(const char *filename) {
    ifstream inpf(filename);
    if (!inpf.good())
	throw se(str("error opening option file", filename));
    for (string line; getline(inpf, line);) {
	auto s = strip(line);
	if (!s.empty())
	    set(s.c_str());
    }
    if (inpf.bad())
	throw se(str("error reading option file", filename));
}

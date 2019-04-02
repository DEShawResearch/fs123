#pragma once

// This is a C++14 parser for INI files. It produces a
// map with section names as keys, and the values are shared_ptr<map> with
// name,value pairs.
// Limitations: does not handle REM comments.
// It should be identical to python3 configparser.ConfigParser
// with inline_comment_prefixes=";#", and strict=False
// It is mostly similar to python2 ConfigParser.ConfigParser
// except for some Py2 oddities/bugs on continuation-line handling
// e.g. python2 does not do inline comments on continuation lines,
// nor indented section names or option values)
// With empty_lines_in_values, handles some things that Py3 throws, sigh!

// Mark Moraes, D. E. Shaw Research

#include <map>
#include <utility>
#include <string>
#include <cstring>
#include <memory>
#include <fstream>
#include <core123/diag.hpp>
#include <core123/strutils.hpp>

namespace core123 {

static const auto _configparser = diag_name("configparser");

// ConfigParser templates take an InterpolationType, a functor class
// which looks up a variable sv in vars, can perform substitution using
// some appropriate syntax on the result (perhaps doing more lookups in vars)
// and return the result.  NoInterpolation does no substitution, PyBasicInterpolation does
// Python-style %(varname)s substitution.

template <typename MapType>
struct NoInterpolation {
    std::string operator()(str_view sv, const MapType& vars, const int max_recursion = 10) {
        return std::string{sv};
    }
};

template <typename MapType>
struct PyBasicInterpolation {
    std::string operator()(str_view sv, const MapType& vars, const int max_recursion = 10) {
        std::string ret;
        std::string::size_type pos = 0;
        DIAG(_configparser, "\"" << sv << "\", max_recursion " << max_recursion);
        while (pos < sv.size()) {
            auto spos = sv.find('%', pos);
            if (spos == std::string::npos) {
                DIAG(_configparser, "no % in \"" << sv.substr(pos) << "\", max_recursion " << max_recursion);
                ret.append(sv.data(), pos, sv.size()-pos);
                break;
            }
            DIAG(_configparser, "found % at " << spos << " in \"" << sv.substr(pos) << "\", max_recursion " << max_recursion);
            spos++;
            if (spos < sv.size()) {
                if (sv[spos] == '%') {
                    ret.append(sv.data()+pos,spos-pos);
                    pos = spos + 1;
                    DIAG(_configparser, "ret after %% is \"" << ret << "\", max_recursion " << max_recursion << " pos " << pos);
                    continue;
                } else if (sv[spos] != '(') { // ')'
                    throw std::runtime_error("PyBasicInterpolation % must be followed by % or ( \""+std::string(sv)+"\""); // ")"
                }
            } else {
                throw std::runtime_error("PyBasicInterpolation end of string after % \""+std::string(sv)+"\""); // ")"
            }
            if (max_recursion <= 0)
                throw std::runtime_error("PyBasicInterpolation cannot recursively interpolate any deeper \""+std::string(sv)+"\"");
            spos++;
            auto epos = sv.find(')', spos);
            DIAG(_configparser, "pos " << pos << " spos " << spos << " epos " << epos);
            if (epos == std::string::npos)
                throw std::runtime_error("PyBasicInterpolation found opening %( but no closing ) in interpolation \""+std::string(sv)+"\"");
            if (epos+1 >= sv.size() || sv[epos+1] != 's')
                throw std::runtime_error("PyBasicInterpolation need s after close paren \"" + std::string(sv)+"\"");
            auto prefix = sv.substr(pos, spos-pos-2);
            std::string name{sv.substr(spos, epos-spos)};
            DIAG(_configparser, "interpolating \"" << name << "\", max_recursion " << max_recursion << " prefix \"" << prefix << "\"");
            auto v = (*this)(vars.at(name), vars, max_recursion-1);
            DIAG(_configparser, "got \"" << v << "\", max_recursion " << max_recursion);
            ret.append(prefix.begin(), prefix.end());
            ret += v;
            pos = epos + 2;
            DIAG(_configparser, "ret now \"" << ret << "\", max_recursion " << max_recursion << " pos " << pos);
        }
        return ret;
    }
};

// ConfigParser template class is the actual INI file parser, by default performs PyBasicInterpolation
// Looks a bit like a mapped type, so one can iterate over sections using a range-for.

// an option is a name=value, each section is a map where .first is name and .second is value
// vars is also this sort of map

namespace impl {
    using CIMap = std::map<std::string, std::string, CILess>;
}

template <typename MapType = impl::CIMap, typename InterpolationType = PyBasicInterpolation<MapType> >
class ConfigParser {
public:
    // can caller set whitespace_, commentchars_, nvsepchars_,
    // defaultsect_, empty_lines_in_values_ before read to
    // modify class behaviour?
    const std::string whitespace_, commentchars_, nvsepchars_, defaultsect_;
    bool empty_lines_in_values_;
    ConfigParser() : whitespace_{" \t\r\n"}, commentchars_{";#"},
        nvsepchars_{":="}, defaultsect_{"DEFAULT"},
        empty_lines_in_values_{true} {}

    using key_type = std::string;
    using mapped_type = std::shared_ptr<MapType>;
    using value_type = std::pair<key_type, mapped_type>;
    using map_type = std::map<key_type, mapped_type>;

    // make ConfigParser look like a map for lookup by section name
    const mapped_type& at(const key_type& s) const { return sections_->at(s); }

    // make ConfigParser look like a map for iteration
    // iterator points to pair of section name, shared_ptr<option_name_value_map>
    typename map_type::const_iterator begin() const { return sections_->cbegin(); }
    typename map_type::const_iterator end() const { return sections_->cend(); }

    // returns list of section names
    std::vector<key_type> get_section_names(void) const {
        std::vector<key_type> ret;
        for (const auto& s : *sections_) {
            ret.push_back(s.first);
        }
        return ret;
    }

    // returns list of default names
    std::vector<key_type> get_default_names(void) const {
        std::vector<key_type> ret;
        for (const auto& s : *defaults_) {
            ret.push_back(s.first);
        }
        return ret;
    }
    // get_default() returns the the value of optname from the DEFAULT section, if it exists, else
    // throws std::out_of_range (since it uses at())
    std::string get_default(const std::string& optname) const {
        return InterpolationType()(defaults_->at(optname), *defaults_);
    }
    // get() returns the value of optname from the specified section, if that option exists,
    // else throws std::out_of_range (since it uses at())
    std::string get(const mapped_type& section, const std::string& optname) const {
        return InterpolationType()(section->at(optname), *section);
    }
    std::string get(const std::string& sectname, const std::string& optname) const {
        auto sp = sections_->at(sectname);
        return InterpolationType()(sp->at(optname), *sp);
    }

    // TODO: reload if changed or just reload, means we have to store mtime/ctime/size
    // to detect change (or appearance of new files in list?)
    int read(const std::string& fname) {
        std::vector<std::string> v{fname,};
        return read(v);
    }
    // returns number of files found and read
    int read(std::vector<std::string>& fnames) {
        auto nsections = std::make_shared<map_type>();
        auto ndefaults = std::make_shared<MapType>();
        int ngood = 0;
        for (const auto& fname : fnames) {
            std::ifstream inpf(fname);
            if (inpf.fail()) {
                DIAG(_configparser, "failed to open file "+fname);
                continue;
            } else if (inpf.bad()) {
                throw std::runtime_error("error opening file "+fname);
            }
            read_(inpf, nsections, ndefaults);
            if (inpf.bad())
                throw std::runtime_error("error reading file "+fname);
            ngood++;
        }
        // after reading all the sections, populate each section with
        // defaults if needed
        for (const auto& nv : *ndefaults) {
            for (auto& sect : *nsections) {
                if (sect.first != defaultsect_) {
                    if (sect.second->find(nv.first) == sect.second->end()) {
                        sect.second->emplace(nv.first, nv.second);
                    }
                }
            }
        }
        // ok, have read all config files, replace current sections_ and defaults_.
        // There is a moment here where sections_ and defaults_ might be inconsistent,
        // so get and get_default from another thread might see different things; unlikely
        // to matter, but if it does, then caller is responsible for locking!
        std::swap(sections_, nsections);
        std::swap(defaults_, ndefaults);
        return ngood;
    }
            
private:
    std::mutex mutex_;
    std::shared_ptr<map_type> sections_;
    mapped_type defaults_;
    void rstrip_inplace_(std::string& s) {
        auto p = s.find_last_not_of(whitespace_);
        if (p != std::string::npos) {
            DIAG(_configparser, "removing trailing whitespace after " << p << " from \"" << s << "\" " );
            s.resize(p+1);
        } else {
            DIAG(_configparser, "removing all whitespace from \"" << s << "\" " );
            s.resize(0);
        }
    }
    str_view remove_comment_(str_view sv) {
        auto svv = sv_lstrip(sv);
        auto cmtpos = svv.find_first_of(commentchars_);
        DIAG(_configparser, "value \"" << svv << "\" cmtpos is " << cmtpos);
        if (cmtpos != std::string::npos &&
            (cmtpos == 0 || whitespace_.find_first_of(svv[cmtpos-1]) != std::string::npos)) {
            DIAG(_configparser, "removing comment " << " \"" << &svv[cmtpos] << "\"");
            svv.remove_suffix(svv.size() - cmtpos);
        }
        return sv_rstrip(svv);
    }
    void read_(std::istream& inpf, std::shared_ptr<map_type>& nsections,
               mapped_type&ndefaults) {
        auto npos = std::string::npos;
        mapped_type cursection;
        std::string curopt;
        size_t nline = 0;
        size_t curindent = 0;
        for (std::string line; std::getline(inpf, line);) {
            auto firstnonws = line.find_first_not_of(whitespace_);
            size_t cpos;
            nline++;
            if (line.size() == 0 || firstnonws == npos) {
                // empty or blank line
                if (empty_lines_in_values_ && cursection && !curopt.empty()) {
                    DIAG(_configparser, nline << ": adding empty line: \"" << cstr_encode(line.c_str()) << "\"");
                    (*cursection)[curopt].append(1, '\n');
                } else {
                    DIAG(_configparser, nline << ": skipping blank line: \"" << cstr_encode(line.c_str()) << "\"");
                }
                continue;
            } else if ((cpos = commentchars_.find_first_of(line[firstnonws])) != npos) {
                // first non-whitespace char is in commentchar, i.e. line comment
                // hmm, should continuation check come before this?
                // NOTE: does not support REM style comments (Py2 does, Py3 does not)
                DIAG(_configparser, nline << ": comment char " << commentchars_[cpos] << " line: " << line);
                continue;
            } else if (firstnonws > curindent && !curopt.empty()) {
                // starts with whitespace, deeper than curindent,
                // prev line was opt, so this is a continuation line
                auto svv = remove_comment_(line);
                DIAG(_configparser, nline << "continuation line: \"" << cstr_encode(line.c_str()) << "\"");
                auto v = (*cursection)[curopt];
                v.append(1, '\n');
                v.append(std::string(svv)); // can just use svv in C++17, maybe C++14 can optimize the alloc away?
                (*cursection)[curopt] = v;
            } else if (line[firstnonws] == '[') {
                // start of section
                if (cursection && !curopt.empty()) rstrip_inplace_((*cursection)[curopt]); // odd "feature": always trim trailing whitespace, even newlines we added
                auto closepos = line.find_first_of(']');
                if (closepos == npos) throw std::runtime_error("did not find closing square bracket for section name in line"+std::to_string(nline)+":"+line);
                if (closepos == firstnonws+1) throw std::runtime_error("null section name in line"+std::to_string(nline)+":"+line);
                std::string sname(&line[firstnonws+1], closepos-firstnonws-1);
                DIAG(_configparser, nline << ": found section name " << sname << "starting at " << firstnonws << " ending at " << closepos << " on line: " << line);
                if (sname != defaultsect_) {
                    auto csp = nsections->find(sname);
                    if (csp == nsections->end()) {
                        cursection = std::make_shared<MapType>();
                        nsections->emplace(sname, cursection);
                    } else {
                        cursection = csp->second;
                    }
                } else {
                    cursection = ndefaults;
                }
                curopt.clear();
                curindent = firstnonws;
            } else {
                // must be an option name=value
                if (!cursection) throw std::runtime_error("cannot process name if not in a section, line: "+std::to_string(nline)+":"+line);
                if (!curopt.empty()) rstrip_inplace_((*cursection)[curopt]); // odd "feature": always trim trailing whitespace, even newlines we added
                auto seppos = line.find_first_of(nvsepchars_);
                if (seppos == npos) throw std::runtime_error("did not find a name value separator, need one of \""+nvsepchars_+"\" in line: "+std::to_string(nline)+":"+line);
                str_view svn{line.data(), seppos};
                svn = sv_strip(svn);
                DIAG(_configparser, nline << ": found name \"" << svn << "\" ending at " << seppos << " on line: " << line);
                auto svv = remove_comment_(line.data() + seppos + 1);
                curopt = std::string(svn);
                curindent = firstnonws;
                (*cursection)[curopt] = std::string(svv);
                DIAG(_configparser, nline << ": set \"" << curopt << "\" = \"" << (*cursection)[curopt] << '\"');
            }
        }
        // fix up final option, if any
        if (cursection && !curopt.empty()) rstrip_inplace_((*cursection)[curopt]); // odd "feature": always trim trailing whitespace, even newlines we added
    }
};

// RawConfigParser does no interpolation.
template <typename MapType = impl::CIMap >
using RawConfigParser = ConfigParser<MapType, NoInterpolation<MapType> >;

}; // namespace core123

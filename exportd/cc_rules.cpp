#include "cc_rules.hpp"
#include <core123/json.hpp>
#include <core123/fdstream.hpp>
#include <core123/unused.hpp>
#include <core123/autoclosers.hpp>
#include <core123/sew.hpp>
#include <core123/scoped_nanotimer.hpp>
#include <core123/diag.hpp>
#include <core123/pathutils.hpp>
#include <core123/stats.hpp>

using namespace core123;

static auto _cc_rules = diag_name("cc_rules");

namespace{
#define STATS_INCLUDE_FILENAME "cc_rules_statistic_names"
#define STATS_STRUCT_TYPENAME cc_rules_stats_t
#include <core123/stats_struct_builder>
cc_rules_stats_t stats;
}

cc_rule_cache::exruleset_sp
cc_rule_cache::read_cc_rulesfile(const std::string& relpath) /*private*/{
    // open relpath relative to export_root.
    // If ENOENT, return {}
    // Otherwise read the contents into an exrulest_sp and return it.
    DIAGkey(_cc_rules, "read_cc_rulesfile(" << relpath << ")\n");
    acfd fd = openat(exrootfd, relpath.c_str(), O_RDONLY);
    if(!fd){
        if(errno == ENOENT){
            stats.cc_rules_enoents++;
            return {}; // an expired null pointer.
        }
        if(errno == ENOTDIR){
            // These should be rare.  They can happen if a directory
            // is deleted, or if a curl request picks a directory "out
            // of thin air".
            stats.cc_rules_enotdirs++;
            return {};
        }
        throw se("read_cc_rulesfile(" + relpath + ")");
    }
    stats.cc_rules_successful_opens++;
    // read the cc_rules from fd.  Expect json like:
    //   {
    //    "rulesfile-maxage": 90,
    //    "re-rules": [
    //       { "re": ".*\\.stk"  ,  "cc": "max-age=1,stale-while-revalidate=1"},
    //       { "re": ".*\\.ark"  ,  "cc": "max-age=10,stale-while-revalidate=10"}
    //     ],
    //    "cc": "max-age=3600,stale-while-revalidate=1800"
    //   }
    //
    // Use Josutis' fdistream to turn our fd into an istream, and use
    // N. Lohmann's json::parse to turn the istream into a json
    // object.  N.b. we're *NOT* dragging in all of boost here.
    // Just one file: thirdparty/include/fdstream.hpp
    atomic_scoped_nanotimer _t(&stats.cc_rules_json_parse_sec);
    auto j = nlohmann::json::parse(boost::fdistream(fd));
    auto rulesfile_maxage{default_ttl};
    auto p = j.find("rulesfile-maxage");
    if( p != j.end())
        rulesfile_maxage = std::chrono::seconds(p->get<int>());
    ruleset_sp ret = std::make_shared<cc_rule_cache::ruleset>();
    if(j.find("re-rules") != j.end()){ // re-rules are optional..
        for(auto& jrer : j.at("re-rules")){
            ret->rerules.emplace_back();
            auto& back = ret->rerules.back();
            back.re = std::regex(jrer.at("re").get<std::string>(), std::regex::extended);
            back.cc = jrer.at("cc").get<std::string>();
        }
    }
    ret->cc = j.at("cc").get<std::string>();
    // json.hpp throws exceptions of type nlohmann::detail::exception,
    // derived from std::exception if it's unhappy.  So if we get
    // here, we have successfully parsed a well-formed json file with
    // keys and types as expected.  WE DO NOT CHECK FOR UNEXPECTED
    // KEYS.
    return expiring<ruleset_sp>(rulesfile_maxage, ret);
}

cc_rule_cache::exruleset_sp
cc_rule_cache::get_cc_rules_recursive(const std::string& path) /*private*/{
    DIAGkey(_cc_rules, "get_cc_rules_recursive(" << path << ")\n");
    exruleset_sp tentative = excache.lookup(path);
    if(tentative && !tentative.expired())
        return tentative;
    auto fpath = path.empty() ? ".fs123_cc_rules" : path + "/.fs123_cc_rules";
    exruleset_sp ret;
    try{
        ret = read_cc_rulesfile(fpath);
    }catch(std::exception& e){
        // How should we react to a borked rules-cache?
        //
        // If we allow the exception to continue unwinding the
        // stack, we ultimately return a 503, which has the
        // undesirable effect of putting clients in a retry loop
        // and tickling failover logic in Varnish.
        //
        // If we throw_with_nested(http_exception(400,...)), clients
        // see an error and the subtree below the borked rules-cache
        // becomes unreachable, but the client doesn't desperately
        // retry and the server doesn't get declared offline.
        //
        // If we catch the exception and fall through after
        // complaining, it's as if the borked rules file never
        // existed.  Nothing will get fixed unless somebody notices
        // the complaints.  But the "breakage" is far less disruptive
        // to clients.
        //
        // June 2018 - let's just complain.
        complain(LOG_WARNING, e, "corrupt rules-cache: " + fpath);
    }
    if(ret){
        excache.insert(path, ret);
        return ret;
    }
    if(path.empty())
        return fallback_cc;
    ret = get_cc_rules_recursive(pathsplit(path).first);
    excache.insert(path, ret);
    return ret;
}

cc_rule_cache::cc_rule_cache(const std::string& export_root, size_t cache_entries, int _default_ttl, const std::string& fb_cc):
    excache(cache_entries),
    default_ttl(_default_ttl),
    fallback_cc(default_ttl, std::make_shared<ruleset>()),
    exrootfd(sew::open(export_root.c_str(), O_DIRECTORY|O_RDONLY))
{
    fallback_cc->cc = fb_cc;
}

std::string
cc_rule_cache::get_cc(const std::string& path_info, bool directory)try{
    atomic_scoped_nanotimer _t(&stats.cc_rules_get_cc_sec);
    std::string pi = directory? path_info : pathsplit(path_info).first;
    ruleset_sp rules = get_cc_rules_recursive(pi);
    for(const auto& rer : rules->rerules){
        if(std::regex_match(path_info, rer.re))
            return rer.cc;
    }
    return rules->cc;
 }catch(std::regex_error& re){
    std::throw_with_nested(std::runtime_error("in get_cc("  + path_info + ") with re.code(): " + std::to_string(re.code())));
 }

std::ostream&
cc_rule_cache::report_stats(std::ostream& os){
    os << stats;
    os << "cc_cache_size: " << excache.size() << "\n"
       << "cc_cache_evictions: " << excache.evictions() << "\n"
       << "cc_cache_hits: " << excache.hits() << "\n"
       << "cc_cache_expirations: " << excache.expirations() << "\n"
       << "cc_cache_misses: " << excache.misses() << "\n";
    return os;
}

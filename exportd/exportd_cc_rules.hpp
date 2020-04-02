#pragma once

#include <string>
#include <regex>
#include <vector>
#include <memory>
#include "fs123/acfd.hpp"
#include <core123/expiring.hpp>
#include <core123/autoclosers.hpp>

struct cc_rule_cache{
    cc_rule_cache(const std::string& export_root, size_t nentries,
                  int _default_ttl, const std::string& fallback_cc);
    std::string get_cc(const std::string& path_info, bool known_directory);
    std::ostream& report_stats(std::ostream& os);
private:
    struct rerule{
        std::regex re;
        std::string cc;
    };

    struct ruleset{
        std::vector<rerule> rerules;
        std::string cc;
    };

    using ruleset_sp = std::shared_ptr<ruleset>;
    using exruleset_sp = core123::expiring<ruleset_sp>;
    core123::expiring_cache<std::string, ruleset_sp>  excache;
    std::chrono::seconds default_ttl;
    exruleset_sp fallback_cc;
    acfd exrootfd;

    exruleset_sp get_cc_rules_recursive(const std::string&);
    exruleset_sp read_cc_rulesfile(const std::string&);
};

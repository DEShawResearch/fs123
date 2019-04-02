#include "selector_manager.hpp"
#include "cc_rules.hpp"
#include "fs123/secret_manager.hpp"
#include <core123/str_view.hpp>
#include <memory>
#include <string>
#include <regex>
#include <utility>

// This is the simple "111" selector_manager I.e., one process/one
//  port/one export_root. So there's only one selector, established at
//  constructor-time, whose data is stored in oneseldata, which we
//  hand out to anyone who asks in selector_match.  From time to time,
//  we expect the event loop to call regular_maintenance, which
//  checks for updates to the "CRF" files by calling
//  regular_maintenance in the one and only oneseldata.

struct content_codec;

struct per_selector111 : public per_selector{
    per_selector111(int sharedkeydir_fd);

    std::shared_ptr<stringtree> longtimeoutroot;
    struct stat ltrsb;
    std::mutex ltrmtx;
    
    // Configuration of regex-based cache-control: DEPRECATED
    std::unique_ptr<std::regex> cc_regex;
    std::string cc_good;
    std::string cc_enoent;

    // Shared secrets?
    std::unique_ptr<secret_manager> sm;

    // decentralized cache control
    std::string short_timeout_cc;
    std::unique_ptr<cc_rule_cache> rule_cache;

    const std::string& basepath() const override;
    const std::string& estale_cookie_src() const override;
    void regular_maintenance() override;

    std::string get_cache_control(const std::string& func, const std::string& path_info, const struct stat*sb, int eno, unsigned max_max_age) override;
    std::string get_encode_secretid() override;
    std::pair<core123::str_view, std::string>
    encode_content(const fs123Req&, const std::string& encode_sid, core123::str_view in,
                   core123::str_view workspace) override;
    std::string decode_envelope(const std::string& ciphertext) override;
    ~per_selector111() override = default;
    std::ostream& report_stats(std::ostream& os) override;
};

struct selector_manager111 : public selector_manager{
    std::shared_ptr<per_selector111> oneseldata;
    selector_manager111(int sharedkeydir_fd);

    std::shared_ptr<per_selector>
    selector_match(const std::string& ) const override {
        return oneseldata;
    }
    void regular_maintenance() override {
        oneseldata->regular_maintenance();
    }

    ~selector_manager111() override = default;
};

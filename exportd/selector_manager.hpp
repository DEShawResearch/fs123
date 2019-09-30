#pragma once

#include "stringtree.hpp"
#include "fs123request.hpp"
#include <core123/str_view.hpp>
#include <string>
#include <memory>
#include <mutex>
#include <sys/stat.h>

// The 'selector_manager' is an abstract class whose concrete
// implementations offer "interesting" logic 
// and decision-making based on the '/sel/ect/or/' part of the URL.
//
// All the interesting stuff is in per_selector.
//
// It's all a bit premature because there's only one concrete
// implementation:  selector_manager111.
struct per_selector;
struct selector_manager{
    // constructor: Called once in main.  Before any threads have had
    // a chance to call selector_match, but after options have been
    // parsed.  May assume that no other threads are calling
    // regular_maintenance or selector_match concurrently
    selector_manager(){}

    // selector_match:  Must be thread-safe.  I.e., selector_match may be called by
    // multiple threads.
    virtual std::shared_ptr<per_selector> selector_match(const std::string& ) const = 0;

    // regular_maintenance: Must be thread-safe.  I.e., it may be
    // called concurrently with other threads calling selector_match.
    // May assume that only one thread will ever call
    // update_selectors.
    virtual void regular_maintenance() = 0;

    virtual ~selector_manager(){};
};

struct per_selector{
    static void validate_estale_cookie(const std::string& value);
    static void validate_basepath(const std::string& bp);
    virtual const std::string& basepath () const = 0;
    virtual const std::string& estale_cookie_src () const = 0;
    virtual void regular_maintenance() = 0;

    virtual std::string get_cache_control(const std::string& func, const std::string& path_info, const struct stat* sb, int eno, unsigned max_max_age) = 0;
    virtual std::string get_encode_secretid() = 0;
    virtual std::pair<core123::str_view, std::string>
    encode_content(const fs123Req&, const std::string& encode_sid, core123::str_view input,
                   core123::str_view workspace) = 0;
    virtual std::string decode_envelope(const std::string& ciphertext) = 0;
    virtual ~per_selector(){}
    virtual std::ostream& report_stats(std::ostream& os) = 0;
};

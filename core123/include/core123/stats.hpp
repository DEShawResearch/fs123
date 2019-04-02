#pragma once

#include <atomic>   // for fmt, needed by stats_builder
#include <ostream>
#include <core123/scoped_nanotimer.hpp>
#include <core123/strutils.hpp> // for fmt, needed by stats_builder

namespace core123 {

// base class from which all classes created by stats_builder are derived.

struct stats_t {
    virtual ~stats_t() {};
    virtual std::ostream& osput(std::ostream& os) const = 0;
    friend std::ostream& operator<<(std::ostream& os, const stats_t& st) {
	return st.osput(os);
    };
};

} // namespace core123

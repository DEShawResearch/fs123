#pragma once

#include <chrono>
#include <cstdint>
#include <fuse/fuse_lowlevel.h>
#include <errno.h>
#include <iosfwd>
#include "backend123.hpp"

// Declarations of things defined in mount.fs123.p7.cpp but that we use
// elsewhere in the client-side code.

reply123 begetattr_fresh(fuse_ino_t ino);
uint64_t validator_from_a_reply(const reply123& r);
std::ostream& report_stats(std::ostream&);
std::ostream& report_config(std::ostream&);
extern int proto_minor;

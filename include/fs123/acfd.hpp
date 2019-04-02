#pragma once
#include <core123/autoclosers.hpp>
#include <core123/complaints.hpp>
#include <exception>
#include <string>

struct fs123_autoclose_err_handler{
    void operator()(const std::exception& e){
        // use LOG_CRIT so it won't be rate-limited.
        core123::complain(LOG_CRIT, std::string("unexpected descriptor autoclose failure: ") + e.what());
    }
};

using acfd = core123::ac::fd_t<fs123_autoclose_err_handler>;
using acDIR = core123::ac::DIR<fs123_autoclose_err_handler>;
using acFILE = core123::ac::FILE<fs123_autoclose_err_handler>;

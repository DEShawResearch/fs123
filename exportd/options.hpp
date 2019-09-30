#pragma once

#include <cstdint>
#include <string>

struct options{
#define OPTION(type, name, default, desc)        \
    type name;
#include "options.inc"
#undef OPTION
    void populate(int argc, char *argv[], int start=1);
};

extern options gopts;

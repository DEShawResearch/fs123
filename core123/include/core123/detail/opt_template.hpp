// note: designed to be be included multiple times for different uses
// so no ifdef or pragma once include protection.

// #define OPT_TEMPLATE(TYPE, DESC, ENUM) to expand to whatever
// code template you want that uses all option types and
// include this file to have your macro expanded for

#ifndef OPT_TEMPLATE
#error OPT_TEMPLATE(TYPE, DESC, ENUM) must be defined to some code
#endif

OPT_TEMPLATE(uint32_t, "UNSIGNED_32BIT_NUMBER", ou32)
OPT_TEMPLATE(uint64_t, "UNSIGNED_64BIT_NUMBER", ou64)
OPT_TEMPLATE(std::string, "STRING", ostr)
OPT_TEMPLATE(double, "DOUBLE_PRECISION_NUMBER", odbl)
OPT_TEMPLATE(int32_t, "SIGNED_32BIT_NUMBER", oi32)
OPT_TEMPLATE(int64_t, "SIGNED_64BIT_NUMBER", oi64)

#undef OPT_TEMPLATE

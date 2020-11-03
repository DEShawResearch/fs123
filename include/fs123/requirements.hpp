#pragma once

// The sole purpose of this file is to give a clear and early error
// message to anyone trying to compile fs123 with a too-old compiler.
// It's #include-ed in a few source files that give wide coverage.

#if defined(__clang__)
// 
#elif defined(__ICC)
// ???
#elif defined(__GNUC__)
#  if __GNUC__ < 6
// N.B.  In general, fs123 makes unconditional use of C++17 features.
// gcc6 does not fully support C++17, but currently (Nov 2020), fs123
// can be compiled with gcc-6.  This may change if we decide to make
// use of a C++17 feature that is not in gcc6 and is not trivially
// worked-around (see the __cpp_lib_optional tests in fs123server.hpp,
// for example).
#    error "fs123 requires at least gcc-6.x and -std=c++17"
#    include "Compilation stopped because fs123 requires at least gcc-6.x and -std=c++17"
#  endif
#endif

#pragma once

// The sole purpose of this file is to give a clear and early error
// message to anyone trying to compile fs123 with a too-old compiler.
// It's #include-ed in a few source files that give wide coverage.

// See docs/Notes.compilers for more info

#if defined(__clang__)
#  if __clang_major__ < 7
#    error "fs123 requires at least clang-7 or gcc-7 and -std=c++17"
#    include "Compilation stopped because fs123 requires at least clang-7 or gcc-8 and -std=c++17"
#  endif
#elif defined(__ICC) // N.B.  icpc also defines __GNUC__, so this must come first
#  if __ICC < 1900
#    error "fs123 requires at least icc-19 or clang-7 or gcc-7 and -std=c++17"
#    include "Compilation stopped because fs123 requires at least icc-19 clang-7 or gcc-8 and -std=c++17"
#  endif
#elif defined(__GNUC__)
#  if __GNUC__ < 7
#    error "fs123 requires at least clang-7 or gcc-7 and -std=c++17"
#    include "Compilation stopped because fs123 requires at least clang-7 or gcc-7 and -std=c++17"
#  endif
#endif

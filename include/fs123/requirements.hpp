#pragma once

// The sole purpose of this file is to give a clear and early error
// message to anyone trying to compile fs123 with a too-old compiler.
// It's #include-ed in a few source files that give wide coverage.

#if defined(__clang__)
// 
#elif defined(__GNUC__)
#  if __GNUC__ < 6
#    error "fs123 requires at least gcc-6.x and -std=c++17"
#    include "Compilation stopped because fs123 requires at least gcc-6.x and -std=c++17"
#  endif
#endif

#pragma once
// http://valgrind.org/docs/manual/drd-manual.html#drd-manual.data-races
// says to put something like this *BEFORE* any C++ header files are
// included

#if defined(VALGRIND_HACKS_FOR_HELGRIND) && __has_include(<valgrind/helgrind.h>)
#include <valgrind/helgrind.h>
#define _GLIBCXX_SYNCHRONIZATION_HAPPENS_BEFORE(addr) ANNOTATE_HAPPENS_BEFORE(addr)
#define _GLIBCXX_SYNCHRONIZATION_HAPPENS_AFTER(addr) ANNOTATE_HAPPENS_BEFORE(addr)
#define _GLIBCXX_EXTERN_TEMPLATE -1
#elif defined(VALGRIND_HACKS_FOR_DRD) && __has_include(<valgrind/drd.h>)
#include <valgrind/drd.h>
#define VALGRIND_HG_DISABLE_CHECKING(x,y)
#define _GLIBCXX_SYNCHRONIZATION_HAPPENS_BEFORE(addr) ANNOTATE_HAPPENS_BEFORE(addr)
#define _GLIBCXX_SYNCHRONIZATION_HAPPENS_AFTER(addr) ANNOTATE_HAPPENS_BEFORE(addr)
#define _GLIBCXX_EXTERN_TEMPLATE -1
#define ANNOTATE_HAPPENS_BEFORE_FORGET_ALL(addr)
#endif

#ifndef ANNOTATE_HAPPENS_AFTER
#define ANNOTATE_HAPPENS_AFTER(x)
#define ANNOTATE_HAPPENS_BEFORE(x)
#define ANNOTATE_HAPPENS_BEFORE_FORGET_ALL(addr)
#define VALGRIND_HG_DISABLE_CHECKING(x,y)
#define VALGRIND_HG_DISABLE_CHECKING(x,y)
#endif


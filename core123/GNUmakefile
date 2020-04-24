# GNU Makefile

# This Makefile will build the core123 library in the directory in which it is run.
# To avoid clutter, it is recommended that you run it somewhere other than the
# top of the core123 tree.  E.g.,
#
#  cd /path/to/build-dir
#  make -C /path/to/checked-out-core123
#
# Everything in this file is fairly generic.  If necessary, use an
# external script to install dependencies and/or set variables like
# CC, CXX, CPPFLAGS, LDLIBS, etc.

mkfile_path := $(word $(words $(MAKEFILE_LIST)),$(MAKEFILE_LIST))
top/ := $(dir $(mkfile_path))

VPATH=$(top/)lib:$(top/)ut

OPT?=-O3 # if not explicitly set
PREFIX?=/usr/local

CPPFLAGS+=-I$(top/)include
CXXFLAGS+=$(OPT)
# N.B.  we use the '#pragma GCC diagnostic ignore  ...' to selectively turn
# off a few warnings.  One of them is -Wint-in-bool-context, which is
# so new that that older compilers complain when we try to ignore it.
# So we addd -Wno-pragmas to keep the older compiler from complaining.
CXXFLAGS+=-Wall -Werror -Wno-pragmas
CXXFLAGS+=-std=c++17
CXXFLAGS+=-ggdb
LDFLAGS+=-L.
TARGET_ARCH+=-pthread

# Note - warning options are completely non-standard.  It's a mess!
#
# To compile the unit tests with icc, use something like:
# CXXFLAGS+=" -wd61"   # integer operation result is out of range (ut_svto.cpp:127, 129, 131)
# CXXFLAGS+=" -wd10006" # -Wno-pragmas in the GNUmakefile.  Ugh.
#
# To compile the unit tests with clang, use something like:
# CXXFLAGS+="-Wno-unknown-warning-option " # -Wno-pragmas in the GNUmakefile
#

ifndef NOLIBRT
LDLIBS+=-lrt # needed to link core123 with glibc<2.17, not harmful otherwise
endif

.PHONY: all
all: check

LINK.o = $(CXX) $(LDFLAGS) $(TARGET_ARCH)

unit_tests:=\
	ut_http_error_category \
	ut_streamutils \
	ut_uuid \
	ut_uchar_span \
	ut_chacha \
	ut_counter_based_engine \
	ut_timeit \
	ut_simd_threefry \
	ut_elastic_threadpool \
	ut_roundtrip \
	ut_demangle \
	ut_stacktrace \
	ut_histogram \
	ut_lguniformhist \
	ut_nonuniformhist \
	ut_intutils \
	ut_threefry_easy \
	ut_threeroe \
	ut_periodic \
	ut_log_channel \
	ut_inserters \
	ut_diagstatic_initializers \
	ut_complaints \
	ut_stats \
	ut_extractors \
	ut_diag \
	ut_nested \
	ut_printfutils \
	ut_rangeInserter \
	ut_svstream \
	ut_diagtime \
	ut_diagtime_flood \
	ut_tupleInserter \
	ut_wrapper \
	ut_yautocloser \
	ut_expiring \
	ut_producerconsumerqueue \
	ut_qp \
	ut_scanint \
	ut_scopedtimer \
	ut_snt \
	ut_svto \
	ut_threadpool \
	ut_pathutils \
	ut_mulhilo \
	ut_threefry \
	ut_philox \
	ut_threefry2x64 \
	ut_philox4x64 \
	ut_datetimeutils \
	ut_strutils \
	ut_envto \
	ut_netstring \
	ut_addrinfo_cache \
	ut_addrinfo_cache2 \
	ut_opt \
	ut_processlines \
	ut_bits \
	ut_bloom

shunit_tests:=ut_configparser

unit_tests.out:=$(addsuffix .out, $(unit_tests) $(shunit_tests))
ut_%.out : ut_%
	./$< > $@.tmp && mv $@.tmp $@

ut_diagtime_flood.out: ut_diagtime_flood
	DIAG_OPTS=flood ./$< > $@.tmp && mv $@.tmp $@

ut_configparser.out: ut_configparser testconfigparser
	TESTDATA=$(top/)/ut/testdata ./$< > $@.tmp && mv $@.tmp $@

.PHONY: check
check: $(unit_tests.out)

.PHONY: install
install:
	[ -n "$(PREFIX)" ] || ( echo PREFIX must be set to install; false )
	mkdir -p $(PREFIX)/include/core123 $(PREFIX)/lib
	cp -a $(top/)include/core123/* $(PREFIX)/include/core123

# Special cases for specific unit tests:

ut_expiring.o : CPPFLAGS += $(VALGRIND_CPPFLAGS)

ut_scanint.o: CXXFLAGS+=-Wno-int-in-bool-context  # Why?  There's a pragma in the code.

ut_diagtime_flood: ut_diagtime_flood.o
ut_diagtime_flood.o: ut_diagtime.cpp
	$(CXX) -c $(CPPFLAGS) -DCORE123_DIAG_FLOOD_ENABLE $(CXXFLAGS) $(TARGET_ARCH) -MD -MP -MF $*.d -MT "$@" $< -o "$@" || (rm -f $*.d $*.o  && false)

ut_diagstatic_initializers : ut_diagstatic_initializersB.o

ut_stats.o ut_stats2.o : CPPFLAGS+=-I$(top/)ut
ut_stats : ut_stats2.o

# We default to BACKWARD_HAS_DW.  To use something different,
# put a -DBACKWARD_HAS_SOMETHING=1 in CPPFLAGS and whatever
# else is needed in LDLIBS.  See ../misc/prereqs.alpine:* for
# possibilities.
ifeq (,$(findstring BACKWARD_HAS,$(CPPFLAGS)))
ut_stacktrace.o : CPPFLAGS += -DBACKWARD_HAS_DW=1
ut_stacktrace : LDLIBS += -ldw
endif

ut_streamutils : LDFLAGS += -pthread

ut_configparser: ut_configparser.sh
	cp -av $< $@

.PHONY: clean
clean:
	rm -f $(unit_tests) $(shunit_tests) *.o *.d *.a *.so *.core ut_*.out ut_*.out.tmp testconfigparser

# the paulandlesley.com autodepends hack, lifted from makefragments,
# but don't do a recursive descent into all subdirs (which would be
# *very* bad if we have a mountpoint running in .!)
%.o: %.cpp
	$(CXX) -c $(CPPFLAGS) $(CXXFLAGS) $(TARGET_ARCH) -MD -MP -MF $*.d -MT "$@" $< -o "$@" || (rm -f $*.d $*.o  && false)

%.o: %.c
	$(CC) -c $(CPPFLAGS) $(CFLAGS) $(TARGET_ARCH) -MD -MP -MF $*.d -MT "$@" $< -o "$@" || (rm -f $*.d $*.o  && false)

# Cancel the no-.o rules.  Always make a .o, and hence always make a .d
%: %.cpp
%: %.c

# All executables need a static-pattern rule to force creation of the .o,
# otherwise the "canceled" %:%.cpp rule seems to be found anyway.
$(unit_tests) : % : %.o

include $(wildcard *.d)

# GNU Makefile

# This Makefile will build the core123 library in the directory in which it is run.
# To avoid clutter, it is recommended that you run it somewhere other than the
# top of the core123 tree.  E.g.,
#
#  cd /path/to/build-dir
#  make -C /path/to/checked-out-core123

# Everything in this file is fairly generic, it is invoked by various dmk-* scripts
# after they set up the environment appropriately.
# If you have all the needed dependencies and the right compiler as CC, CXX,
# this may just work using make

mkfile_path := $(word $(words $(MAKEFILE_LIST)),$(MAKEFILE_LIST))
top/ := $(dir $(mkfile_path))

VPATH=$(top/)lib:$(top/)ut

OPT?=-O3 # if not explicitly set
PREFIX?=/usr/local

CPPFLAGS+=-I$(top/)include
CXXFLAGS+=-Wno-deprecated-declarations
CXXFLAGS+=$(OPT)
CXXFLAGS+=-Wall
CXXFLAGS+=-std=c++17
ifndef NOLIBRT
LDLIBS+=-lrt # needed to link core123 with glibc<2.17, not harmful otherwise
endif

LINK.o = $(CXX) $(LDFLAGS) $(TARGET_ARCH)

unit_tests:=\
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

# ut_configparser is a shell script.  It needs test_configparser to run, but
# it is not compiled, so use the | construct.
ut_configparser : | testconfigparser

exe:=$(unit_tests) $(shunit_tests)

all: $(exe)
.PHONY: all

.PHONY: check
check: runtests

.PHONY: install
install:
	[ -n "$(PREFIX)" ] || ( echo PREFIX must be set to install; false )
	mkdir -p $(PREFIX)/include/core123 $(PREFIX)/lib
	cp -a $(top/)include/core123/* $(PREFIX)/include/core123

# XXX should we build unit tests with static too? instead?

# Unit tests are in ut
$(exe): LDFLAGS+=-L.
$(exe): LDFLAGS+=-pthread
$(addsuffix .o, $(exe)): CXXFLAGS+=-ggdb

ut_expiring.o : CPPFLAGS += $(VALGRIND_CPPFLAGS)

ut_svto.o ut_scanint.o: CPPFLAGS+=-Wno-int-in-bool-context

ut_configparser: ut_configparser.sh
	cp -av $< $@

ut_diagtime_flood: ut_diagtime_flood.o
ut_diagtime_flood.o: ut_diagtime.cpp
	$(CXX) -c $(CPPFLAGS) -DCORE123_DIAG_FLOOD_ENABLE $(CXXFLAGS) $(TARGET_ARCH) -MD -MP -MF $*.d -MT "$@" $< -o "$@" || (rm -f $*.d $*.o  && false)

ut_diagstatic_initializers : ut_diagstatic_initializersB.o

ut_stats.o ut_stats2.o : CPPFLAGS+=-I$(top/)ut
ut_stats : ut_stats2.o

#ut_stacktrace.o : CPPFLAGS += -DBACKWARD_HAS_BFD=1
#ut_stacktrace : LDLIBS += -lbfd -ldl
ut_stacktrace.o : CPPFLAGS += -DBACKWARD_HAS_DW=1
ut_stacktrace : LDLIBS += -ldw

ut_streamutils : LDFLAGS += -pthread

.PHONY: runtests
runtests: $(exe) $(shunit_tests)
	for t in $(unit_tests) $(shunit_tests); do echo Running $$t; DIAG_OPTS=flood TESTDATA=$(top/)ut/testdata ./$$t || exit 1; echo Finished $$t; done

.PHONY: clean
clean:
	rm -f $(exe) *.o *.d *.a *.so *.core

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

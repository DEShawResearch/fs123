# This Makefile assumes that fs123's prerequisites are met.
# See README.md for the prerequisites.
#
# Tell make where to find binaries and libraries installed in
# non-standard locations by setting environment variables: CC, CXX,
# TARGET_ARCH, CPPFLAGS, CXXFLAGS, CFLAGS, LDFLAGS, LDLIBS.
#
# Targets and (many!) intermediate files will be created in the
# directory from which make is run.  Out-of-tree builds are supported.
# So to avoid cluttering the source tree, 
#   mkdir build
#   cd build
#   make -f ../GNUmakefile
#
# See http://make.mad-scientist.net/papers/multi-architecture-builds/
# for a possible alternative...
.SECONDARY:
mkfile_path := $(word $(words $(MAKEFILE_LIST)),$(MAKEFILE_LIST))
top/ := $(dir $(mkfile_path))
abstop/ := $(realpath $(top/))/
VPATH=$(top/)lib:$(top/)client:$(top/)exportd:$(top/)examples
# Link with $(CXX), not $(CC)!
LINK.o = $(CXX) $(LDFLAGS) $(TARGET_ARCH)

# First, let's define the 'all' target so a build with no arguements
# does something sensible:
binaries=fs123p7

unit_tests=ut_diskcache
unit_tests += ut_seektelldir
unit_tests += ut_content_codec
unit_tests += utx_cc_rules
unit_tests += ut_inomap

# other_exe
other_exe = ex1server

libs=libfs123.a

EXE = $(binaries) $(unit_tests) $(other_exe)

.PHONY: all
all: $(EXE)

.PHONY: check
check: $(EXE)
	$(top/)test/runtests

.PHONY: core123-check
core123-check:
	-mkdir core123-build
	$(MAKE) -C core123-build -f $(abstop/)core123/GNUmakefile check

OPT?=-O3 # if not explicitly set
DASHG?=-ggdb
CPPFLAGS += -iquote $(top/)include
CPPFLAGS += -I $(top/)core123/include
CXXFLAGS += -DFUSE_USE_VERSION=26
CXXFLAGS += -std=c++17 -Wall
CXXFLAGS += -Wshadow
CXXFLAGS += -Werror
CXXFLAGS += -Wextra
CXXFLAGS += -D_FILE_OFFSET_BITS=64
CXXFLAGS += $(OPT)
CXXFLAGS += $(DASHG)

CFLAGS += -std=c99
CFLAGS += $(OPT)
CFLAGS += $(DASHG)
LDFLAGS += -pthread

GIT_DESCRIPTION?=$(shell cd $(top/); git describe --always --dirty || echo not-git)
CXXFLAGS += -DGIT_DESCRIPTION=\"$(GIT_DESCRIPTION)\"

serverlibs=-levent -levent_pthreads -lsodium

# < libfs123 >
libfs123_cppsrcs:=content_codec.cpp secret_manager.cpp sharedkeydir.cpp fs123server.cpp
CPPSRCS += $(libfs123_cppsrcs)
libfs123_objs:=$(libfs123_cppsrcs:%.cpp=%.o)
libfs123.a : $(libfs123_objs)
	$(AR) $(ARFLAGS) $@ $?

# ut_content_codec needs libsodium
ut_content_codec : LDLIBS += -lsodium

# < /libfs123 >

# <client fs123p7>
fs123p7_cppsrcs:=fs123p7.cpp app_mount.cpp app_setxattr.cpp app_ctl.cpp fuseful.cpp backend123.cpp backend123_http.cpp diskcache.cpp special_ino.cpp inomap.cpp openfilemap.cpp distrib_cache_backend.cpp
fs123p7_cppsrcs += app_exportd.cpp exportd_handler.cpp exportd_cc_rules.cpp
CPPSRCS += $(fs123p7_cppsrcs)
fs123p7_objs :=$(fs123p7_cppsrcs:%.cpp=%.o)

fs123p7_csrcs:=opensslthreadlock.c
CSRCS += $(fs123p7_csrcs)
fs123p7_objs +=$(fs123p7_csrcs:%.c=%.o)

ifndef NO_OPENSSL
fs123p7 : LDLIBS += -lcrypto
endif
fs123p7 : LDLIBS += -lsodium
FUSELIB?=fuse # if not  explicitly set
fs123p7 : LDLIBS += -l$(FUSELIB) -ldl # -ldl is needed for static linking.  It's harmless otherwise, but dpkg-shlibdeps warns that it's a 'useless dependency'
fs123p7 : LDLIBS += $(shell curl-config --libs) 
fs123p7 : LDLIBS += $(serverlibs)
fs123p7 : $(fs123p7_objs)

LDLIBS += -lz # -lz is needed for static linking.  It's harmless otherwise, but dpkg-shlibdeps warns that it's a 'useless dependency'

# link ut_diskcache links with some client-side .o files
ut_diskcache : diskcache.o backend123.o 
ut_inomap : inomap.o

backend123_http.o : CPPFLAGS += $(shell curl-config --cflags)
#</client>


# <ex1server>
ex1_cppsrcs := ex1server.cpp
CPPSRCS += $(ex1_cppsrcs)
ex1_objs := $(ex1_cppsrcs:%.cpp=%.o)
ex1server: LDLIBS += $(serverlibs)
ex1server: $(ex1_objs)
	$(LINK.o) $^ $(LOADLIBES) $(LDLIBS) -o $@
# </ex1server>

# Why doesn't this work if it's higher up?
$(EXE): libfs123.a

.PHONY: clean
clean:
	rm -f $(EXE) *.o *.gcno *.gcda *.gcov *.a *.so
	[ ! -d core123-build ] || rm -rf core123-build
	[ ! -d "$(DEPDIR)" ] || rm -rf $(DEPDIR)

export prefix?=/usr
export bindir?=$(prefix)/bin
export sbindir?=$(prefix)/sbin
export libdir?=$(prefix)/lib
export includedir?=$(prefix)/include

.PHONY: install
install : $(binaries) $(libs)
	mkdir -p $(DESTDIR)$(includedir) $(DESTDIR)$(libdir) $(DESTDIR)$(bindir) $(DESTDIR)$(sbindir)
	cp -a $(binaries) $(DESTDIR)$(bindir)
	ln -s $(bindir)/fs123p7 $(DESTDIR)$(sbindir)/mount.fs123p7
	cp -a $(libs) $(DESTDIR)$(libdir)
	cp -a $(top/)include/fs123 $(DESTDIR)$(includedir)
	$(MAKE) -f $(top/)/core123/GNUmakefile install

# <autodepends from http://make.mad-scientist.net/papers/advanced-auto-dependency-generation>
# Modified to work with CSRCS and CPPSRCS instead of just SRCS...
DEPDIR := .deps
DEPFLAGS = -MT $@ -MMD -MP -MF $(DEPDIR)/$*.d

COMPILE.c = $(CC) $(DEPFLAGS) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -c

%.o : %.c
%.o : %.c $(DEPDIR)/%.d | $(DEPDIR)
	$(COMPILE.c) $(OUTPUT_OPTION) $<

COMPILE.cpp = $(CXX) $(DEPFLAGS) $(CXXFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -c
%.o : %.cpp
%.o : %.cpp $(DEPDIR)/%.d | $(DEPDIR)
	$(COMPILE.cpp) $(OUTPUT_OPTION) $<

$(DEPDIR): ; @mkdir -p $@

DEPFILES := $(CSRCS:%.c=$(DEPDIR)/%.d) $(CPPSRCS:%.cpp=$(DEPDIR)/%.d)
$(DEPFILES):
include $(wildcard $(DEPFILES))
# </autodepends>

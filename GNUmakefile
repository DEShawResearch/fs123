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

mkfile_path := $(word $(words $(MAKEFILE_LIST)),$(MAKEFILE_LIST))
top/ := $(dir $(mkfile_path))
VPATH=$(top/)lib:$(top/)client:$(top/)exportd
# default PREFIX if none specified
PREFIX?=/usr/local

# First, let's define the 'all' target so a build with no arguements
# does something sensible:
binaries=fs123p7
binaries+=fs123p7exportd

unit_tests=ut_diskcache
unit_tests += ut_seektelldir
unit_tests += ut_content_codec
unit_tests += utx_cc_rules

EXE = $(binaries) $(unit_tests)

.PHONY: all
all: $(EXE)

.PHONY: check
check: $(EXE)
	$(top/)test/runtests

OPT?=-O3 # if not explicitly set
CPPFLAGS += -iquote $(top/)include
CPPFLAGS += -I $(top/)core123/include
CXXFLAGS += -Wno-deprecated-declarations
CXXFLAGS += -DFUSE_USE_VERSION=26
CXXFLAGS += -std=c++17 -ggdb -Wall -Wshadow -Werror
CXXFLAGS += -Wextra
CXXFLAGS += $(OPT)
CXXFLAGS += -D_FILE_OFFSET_BITS=64
CFLAGS += -std=c99 -ggdb
CFLAGS += $(OPT)
LDFLAGS += -pthread
LDFLAGS += -L. # so the linker can find ./libfs123.a
LDLIBS += -lfs123

# git desribe --exclude would be b
GIT_DESCRIPTION?=$(cd $(top/) shell git describe --always --dirty || echo not-git)
CXXFLAGS += -DGIT_DESCRIPTION=\"$(GIT_DESCRIPTION)\"

# libfs123.a:
# code shared by client and server, and that might be useful for
# other servers
libs:=libfs123.a
$(EXE): | $(libs)
libobjs:=content_codec.o secret_manager.o sharedkeydir.o
libfs123.a : $(libobjs)
	$(AR) $(ARFLAGS) $@ $?

# fs123p7: the client, linked into a single binary with a few utilities
fs123p7obj := fs123p7.o app_mount.o app_setxattr.o app_ctl.o fuseful.o backend123.o backend123_http.o diskcache.o special_ino.o inomap.o opensslthreadlock.o openfilemap.o
ifndef NO_OPENSSL
fs123p7 : LDLIBS += -lcrypto
endif
fs123p7 : LDLIBS += -lsodium
FUSELIB?=fuse # if not  explicitly set
fs123p7 : LDLIBS += -l$(FUSELIB) -ldl # -ldl is needed for static linking.  Should be harmless otherwise
fs123p7 : LDLIBS += $(shell curl-config --libs) -lz # -lz is needed for static linking.  Should be harmless otherwise
fs123p7 : $(fs123p7obj)

backend123_http.o : CPPFLAGS += $(shell curl-config --cflags)

# fs123p7exportd: the server
serverobj := crfio.o stringtree.o selector_manager.o selector_manager111.o do_request.o fs123request.o fs123p7exportd_common.o cc_rules.o options.o
fs123p7exportd : LDLIBS+=-levent -lsodium
fs123p7exportd : $(serverobj)

# A few unit tests need a bit of extra help:
ut_diskcache : diskcache.o backend123.o 
ut_diskcache : LDLIBS += -lsodium
ut_content_codec : LDLIBS += -lsodium
utx_cc_rules : cc_rules.o

# Boilerplate
LINK.o = $(CXX) $(LDFLAGS) $(TARGET_ARCH)

.PHONY: clean
clean:
	make -f $(top/)core123/GNUmakefile clean
	rm -f $(EXE) *.o *.d *.gcno *.gcda *.gcov *.a *.so

.PHONY: install
install : $(binaries) $(libs)
	mkdir -p $(PREFIX)/include $(PREFIX)/lib $(PREFIX)/bin
	cp -a $(binaries) $(PREFIX)/bin
	cp -a $(libs) $(PREFIX)/lib
	cp -a $(top/)include/fs123 $(PREFIX)/include

# the paulandlesley.com autodepends hack, lifted from makefragments,
# but don't do a recursive descent into all subdirs (which would be
# *very* bad if we have a mountpoint running in .!)
%.o: %.cpp
	$(CXX) -c $(CPPFLAGS) $(CXXFLAGS) $(TARGET_ARCH) -MD -MP -MF $*.d -MT "$@" $< -o "$@" || (rm -f $*.d $*.o  && false)

# Cancel the no-.o rules.  Always make a .o, and hence always make a .d
%: %.cpp
%: %.c

include $(wildcard *.d)

# Core123

Core123 is a motley collection useful of C++ libraries and utilities.
Almost all are header-only, but one (complaints.hpp) require linkage.
To use the header-only utilities, just #include the relevant header
and go.  The only headers that require linkage are explicitly
identified in the list below.

Each header file contains a more-or-less standalone collection of
related classes and functions.  Documentation is in comments at
the top of the file.

They make extensive, unconditional use of C++14.  There is conditional
use of C++17.

Most of the code is original, but a few libraries are available
elsewhere and are included here under the original authors' terms
(e.g., an MIT license).

One header file (threeroe) also has python bindings.

# Build and installation

A GNUmakefile is provided to build the library.  E.g.,

```bash
make
make check
make PREFIX=/path/to/prefix install
```
The install rule requires that PREFIX be set, and creates and
populates include/ and lib/ sub-directories under PREFIX.

The GNUmakefile leaves targets and intermediate files in the
directory from which it is run, but it also supports out-of-tree
builds.  To avoid clutter, consider cd-ing to a build directory
before calling make, e.g.,
```bash
mkdir build
cd build
make -f ../GNUmakefile check
```

# General categories:

## Streams, strings and I/O

* const_ntbsExtractor.hpp
* svstream.hpp
* syslogstream.hpp
* scanint.hpp
* svto.hpp
* envto.hpp
* netstring.hpp
* pathutils.hpp
* strutils.hpp
* streamutils.hpp
* json.hpp  (MIT license)
* base64.hpp (MIT license)
* fdstream.hpp (MIT license)
* configparser.hpp
* opt.hpp
* processlines.hpp

## Threads and synchronization

* producerconsumerqueue.hpp
* threadpool.hpp
* countedobj.hpp
* occupancyguard.hpp
* periodic.hpp

## Miscellaneous

* unused.hpp
* named_ref.hpp
* fwd_capture.hpp
* addrinfo_cache.hpp
* intutils.hpp
* bits.hpp
* bloomfilter.hpp

## Errors, exceptions and diagnostics

* sew.hpp
* autoclosers.hpp
* throwutils.hpp
* nested_exception.hpp
* syslog_number.hpp
* stacktrace.hpp
* backward.hpp        (MIT license)
* diag.hpp
* complaints.hpp      (requires linkage)
* log_channel.hpp
* http\_error\_category.hpp

## Counters and Timers

* stats.hpp
* stats\_struct\_builder (see stats.hpp, unconventional #include strategy)
* scoped_timer.hpp
* scoped_nanotimer.hpp
* timeit.hpp
* datetimeutils.hpp

## Hashes and random numbers

* threeroe.hpp
* counter\_based\_urng.hpp
* threefry.hpp
* philox.hpp

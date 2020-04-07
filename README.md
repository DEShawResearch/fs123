# fs123

`fs123` is a scalable, high-performance, network filesystem.

A simple web protocol with just 6 request types
leverages the entire HTTP ecosystem (load balancers,
proxies, redirects, etc.) and makes it easy to implement and
debug custom file servers.

`fs123` provides consistency guarantees
with path-specific, configurable timeouts, which enables
aggressive in-kernel, local-disk, and shared http-proxy
caching of positive and negative accesses. It accelerates PATH
or PYTHONPATH searches, and is WAN/cloud-friendly.

The client and server are written in C++. Each is a single
user-space binary that does not require root privileges.
The client uses the FUSE low-level API, and can work through
network outages (or even offline) once the on-disk cache is
primed.  The libevent-based server easily delivers data
at 40Gbps.

# Build and installation

## Prerequisites

We run fs123 in production on CentOS7 using a mix of CentOS-provided
libraries (libfuse, libcurl, libsodium) and tools andlibraries that we
have built and installed from sources (gcc6, libevent)

We have successfully run fs123's self-tests in docker images running
Ubuntu 18.04(LTS) bionic, Ubuntu 16.04(LTS) xenial, Fedora 29 and
CentOS7 using the distro's provided compilers and libraries.  See the
scripts in `misc/prereqs.*` for hints about how to install fs123's
prerequisites in different distros.

The prerequisites to compile and link the client and server are:

- gcc
    10.0.1 (Fedora32), 9.2.0 (alpine 3.11.5), 8.3.1 (Fedora29), 8.2.0 (bionic), 9.2.1 (xenial), devtoolset-8-gcc-8.2.1 (CentOS7) have been tested. Any compiler with full C++17 support should work.
- gmake
    3.82 has been tested.
- libfuse
    2.9.9 (on Fedora29), 2.9.8 (alpine 3.11.5), 2.9.7 (on bionic), 2.9.4 (on xenial), 2.9.2 (on CentOS7) have been tested
- libsodium
    1.10.18 (on Fedora32, alpine 3.11.5), 1.0.17 (on Fedora29, CentOS7), 1.0.16 (on bionic), 1.0.8 (on xenial) have been tested
- libcurl
    7.69 (on Fedora32), 7.67 (on alpine 3.11.5), 7.61 (on Fedora29), 7.58.0 (on bionic), 7.43.0 (on xenial), 7.19.0 (on CentOS7) have been tested
- libevent
    2.1.11 (on alpine 3.11.5), 2.1.8 (on Fedora32, Fedora29, bionic), 2.0.21 (on xenial, CentOS7) have been tested
    
Although only a few combinations have been tested, it's very likely that other combinations will work.

To run the tests (invoked by make check) the following are needed.  The standard distro versions should be fine:

- valgrind
- time 
- attr
- curl
- python3
- e2fsprogs

## Compiling

A GNUmakefile is provided.  There are (currently) no autoconf or
other meta-build tools.  
To build, test and install the client, server, and libraries common to both:

```bash
make
make check
make install
```

The install rule defaults to installing binaries in /usr/local, but
the destination can be changed by setting PREFIX on the command line,
e.g.,


```bash
make PREFIX=/absolute/path install
```

The GNUmakefile leaves targets and intermediate files in the
directory from which it is run, but it also supports out-of-tree
builds.  To avoid clutter, consider cd-ing to a build directory
before calling make, e.g.,
```bash
$ mkdir build
$ cd build
$ make -f ../GNUmakefile check
```

The Makefile follows standard conventions
for CXX, CC, TARGET_ARCH, CPPFLAGS, CFLAGS, CXXFLAGS, LDFLAGS, and LDLIBS.
If prerequisites are installed in places that are not
automatically found by your toolchain, set one or more environment
variables before calling make.  E.g., on xenial 16.04, one can use
gcc6 from the ppa:ubuntu-toolchain-r/test repository, with:
```bash
$ env CC=gcc-6 CXX=g++-6 make -f .../GNUmakefile check
```

# Running

## Server

To start the server, run
```bash
$ fs123p7 exportd --log_destination %stderr --port 4321 --chroot=  --export_root=/some/where
N[0.0]+0.000 complaint_delta_timestamp start time: 1554218181.000765 2019-04-02 15:16:21+0000
N[1.0]+0.001 main thread started on 127.0.0.1:4321 at 0.000358426
```

By default, the export daemon runs as a normal foreground process.  Job-control works
in the usual ways with nohup, &, bg, wait, etc.  It can be fully disconnected
from the calling terminal (i.e., daemonized) with the `--daemonize` option.  If `--log-destination`
is not specified, logs go to the LOG_USER syslog facility.

It's best if the --export_root is on a filesystem that supports the
FS_IOC_GETVERSION ioctl, e.g., ext3, ext4, xfs or most other local
disk-backed filesystems, but NOT tmpfs or nfs or docker overlay
mounts.  If you see warnings like this:
```bash
W[4.0]+128.333 do_estale_cookie_common_(fullpath=/exported)
W[4.1] ioctl(17, -2146929151, 0x7f19f846208c): Inappropriate ioctl for device
```
it means that FS_IOC_GETVERSION is not supported, in which case, add
`--estale_cookie_src=st_ino` to the command line.

By default, the export daemon will bind the 127.0.0.1 address, and therefore will
only be accessible to clients on localhost.  To make it accessible remotely, add
`--bindaddr=0.0.0.0` to the command line.


Check that the port is working:
```bash
$ curl -v http://localhost:4321/fs123/7/2/a
*   Trying 127.0.0.1...
* TCP_NODELAY set
* Connected to localhost (127.0.0.1) port 4321 (#0)
> GET /fs123/7/2/a HTTP/1.1
> Host: localhost:4321
> User-Agent: curl/7.58.0
> Accept: */*
> 
< HTTP/1.1 200 OK
< fs123-estalecookie: 0
< Cache-Control: public,max-age=60,stale-while-revalidate=30
< Date: Tue, 02 Apr 2019 15:20:50 GMT
< fs123-errno: 0
< Content-Type: application/octet-stream
< fs123-trsum: b2235900f57ebbd229db1741bd0e2232
< Content-Length: 120
< 
16877 2 0 0 4096 1554218149 1554218149 1554218140 85513372 681248835 681248835 746338405 60 8 4096 0
* Connection #0 to host localhost left intact
1554218149681248835$
```
The reply should be a 200, with a Cache-control header, an fs123-errno: 0 header,
and a body consisting of numerical values corresponding to the values in the export_root's
stat.

## Client
Now create a mount-point and start up a client.
```bash
$ mkdir /tmp/mtpt
$ fs123p7 mount http://localhost:4321 /tmp/mnt
$ ls /tmp/mnt
... <contents of export root> ...
$ cd /tmp/mnt
$ ...
```

If you started `fs123p7 exportd` with `--bindaddr=0.0.0.0`, then you should be able
to mount the exported filesystem on another machine, e.g.,
```bash
$ mkdir /tmp/mnt
$ fs123p7 mount http://HOSTNAME:4321 /tmp/mnt
$ ls /tmp/mnt
... <contents of export root> ...
$ cd /tmp/mnt
$ ...
```
# Documentation

Documentation is a work-in-progress.  The fs123p7 binary has a
--help option, which give a complete list of options with
extremely cursory descriptions.  Files in the docs/ directory describe
certain aspects of fs123 in great detail, but there are large gaps,
and occasional anachronisms.

%{!?python_sitelib: %define python_sitelib %(%{__python} -c "from distutils.sysconfig import get_python_lib; print get_python_lib()")}

Name:           fs123p7
Version:        @@VERSION@@
Release:        1%{?dist}
Summary:        Client and server for fs123 protocol=7: a read-only filesystem over http transport.
Vendor:         D. E. Shaw Research, LLC

Group:          Applications/System
License:        Proprietary
Source0:        %{name}-%{version}.tar.gz
BuildRoot:      %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)

BuildRequires:  make
BuildRequires:  devtoolset-8-toolchain
BuildRequires:  libcap-devel
BuildRequires:  fuse-devel
BuildRequires:  openssl-devel
BuildRequires:  libsodium-devel
BuildRequires:  libcurl-devel
BuildRequires:  libevent-devel

Requires: fuse

%description

fs123 is a read-only fuse filesystem optimized for mounts over
wide-area networks.  The fs123 client allows the local kernel to
retain objects in its own caches, and avoids the vast majority of
re-validations (and hence network round-trips).

Network transport use HTTP/1.1, and cache timeout information is
carried in standard HTTP Cache-control headers.  These headers
have the standard meaning, and are therefore compatible with
http proxy caches such as squid and varnish.

A single binary, fs123p7, serves multiple roles (server, client and
various diagnostic and administrative tasks), dispatching on its first
'command' argument.

When run as a client, 'fs123p7 mount' understands conventional mount
option syntax (-o option1,option2=value).  A copy of fs123p7 is
installed in /sbin/mount.fs123p7, facilitating use of the client via
automount or /etc/fstab.

When run as a server, 'fs123p7 exportd' allows clients a read-only
view of a directory tree on the server.

%prep
%setup -q 

%build
. /opt/rh/devtoolset-8/enable
GIT_DESCRIPTION=%{version}-rpm LDFLAGS=-Wl,--build-id make %{?_smp_mflags}

# Man pages, use a2x to convert .txt (asciidoc) to man format
#(cd doc/man; make)

%install
. /opt/rh/devtoolset-8/enable
rm -rf $RPM_BUILD_ROOT
GIT_DESCRIPTION=%{version}-rpm PREFIX=%{_prefix}  %make_install
# FIXME - the makefile should undertand prefix, bindir, libdir, etc.
# On 64-bit RH systems _libdir is /usr/lib64, *not* /usr/lib, but since
# the makefile doesn't grok _libdir, we have to fix it here.
%{__mkdir_p} $RPM_BUILD_ROOT%{_libdir} $RPM_BUILD_ROOT%{_sbindir}
mv $RPM_BUILD_ROOT%{_prefix}/lib/libfs123.a $RPM_BUILD_ROOT%{_libdir}/libfs123.a
# FIXME - the makefile should deal with %{_sbindir}/mount.fs123p7
%{__mkdir_p} $RPM_BUILD_ROOT%{_sbindir}
%{__ln_s} %{_bindir}/fs123p7 $RPM_BUILD_ROOT%{_sbindir}/mount.fs123p7
# FIXME - the makefile shouldn't install ex1server
%{__rm} $RPM_BUILD_ROOT%{_bindir}/ex1server

# when we finally get man pages:
#for chap in 1 5; do
#    %{__mkdir_p} -m0755 $RPM_BUILD_ROOT%{_mandir}/man${chap}
#    %{__install} doc/man/*.$chap.gz $RPM_BUILD_ROOT%{_mandir}/man${chap}
#done
# FIXME - what about the docs/ and examples/?

%clean
rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root,-)
%caps(cap_dac_override,cap_sys_chroot=p) %{_bindir}/fs123p7
%{_sbindir}/mount.fs123p7

%package devel
Summary: C++ library that supports creation of new fs123 servers.
%description devel
C++ library that encapsulates many of the details
of the fs123 protocol.  It is especially useful for
creating new fs123 servers.

Also includes the core123 collection of header-only C++ libraries.

Each header file in /usr/include/core123 contains a more-or-less
standalone collection of related classes and functions.
Documentation is in comments at the top of the file.

They make extensive, unconditional use of C++14.  There is limited
use of C++17.

Most of the core123 code is original, but a few libraries are
available elsewhere and are included here under the original authors'
terms (e.g., an MIT license).

%files devel
%{_prefix}/include/fs123
%{_prefix}/include/core123
# the makefile installs to /usr/lib.  Move it to /usr/lib64.
%{_libdir}/libfs123.a

%changelog
* Tue Sep 08 2020 John Salmon <salmonj@deshawresearch.com> - 10
- conversion from desres-internal spec file


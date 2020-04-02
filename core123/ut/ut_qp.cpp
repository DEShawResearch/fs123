// Test quoted-printable encoding of first MiB from a file by feeding
// the encoded qp to python to decode and reading that back, compare
// with original text.

#include "core123/strutils.hpp"
#include "core123/diag.hpp"
#include "core123/sew.hpp"
#include "core123/autoclosers.hpp"
#include "core123/ut.hpp"
#include <string>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <errno.h>

const char *prog;
const size_t testsize = 1024*1024;

namespace sew = core123::sew;

void dotest(const char *fname) {
    core123::ac::FILE<> fp = sew::fopen(fname, "rb");
    // cbuf is just RAII buffer space for buf (for fread),
    // avoid just declaring buf as a 1M array on the stack
    // because 1M on the stack causes valgrind to complain
    // or require --max-stackframe=2097520
    std::unique_ptr<char[]>cbuf(new char[testsize]);
    auto buf = cbuf.get();
    auto nf = sew::fread(buf, 1, sizeof(buf), fp);
    auto s = core123::quopri(core123::str_view(buf, nf));
    DIAGf(_ut, "%zu bytes from %s became %zu after quopri\n",
             nf, fname, s.size());
    char tname[] = "/tmp/ut_qp_XXXXXX";
    auto tf = sew::mkstemp(tname);
    auto nb = sew::write(tf, s.data(), s.size());
    sew::fsync(tf);
    DIAGf(_ut, "wrote %zd bytes to tempfile %s\n", nb, tname);
    std::string pycmd("python3 -c 'import sys, binascii; sys.stdout.buffer.write(binascii.a2b_qp(sys.stdin.read()))' < ");
    pycmd += tname;
    core123::ac::PIPEFILE<> p = sew::popen(pycmd.c_str(), "r");
    std::unique_ptr<char[]>cbuf2(new char[testsize]);
    auto buf2 = cbuf2.get();
    auto nf2 = sew::fread(buf2, 1, sizeof buf2, p);
    DIAGf(_ut, "read %zu bytes from command\n%s\n", nf2, pycmd.c_str());
    EQUAL(nf, nf2);
    EQSTR(std::string(buf, nf), std::string(buf2, nf));
    sew::remove(tname);
}

int
main(int argc, char **argv) {
    const char *fname;
    prog = argv[0];
    if (argc == 1) {
        fname = "/bin/bash";
    } else if (argc == 2) {
        fname = argv[1];
    } else {
        fprintf(stderr, "Usage: %s [FILENAME]\nwill generate quoted-printable for first 1MiB of filename, and verify it via Python decode, uses /vmlinuz of no FILENAME is provided\n",
                prog);
        return 1;
    }
    dotest(fname);
    return utstatus();
}

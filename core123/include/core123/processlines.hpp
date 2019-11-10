// processlines is a function to quickly read (newline-delimited) lines
// from any of an istream, FILE * or fd, or from an arbitrary lambda fillbuf.
// <ssize_t(char *buf, size_t bufsize)> which must return 0 on eof or
// the number of bytes read into buf <= the bufsize argument.
// processlines will call the provided lambda f on each line (as a str_view)
// It minimizes data copies by using passing str_views
// into a large buffer, which f can only treat as valid till it returns,
// since the underlying buffer may get moved around.
// At the moment, it does not attempt to resize the buffer to handle
// extra-long lines, perhaps one-day...
// With default 16KiB bufsize, processlines takes 0.5 sec to count
// 10M input records totalling 1Gbyte, vs 0.9 sec for C or C++ getline
// on a 2.7Ghz i7-7500U, gcc 6.3.0 -O3, NVME disk.
// (getline() can be over 10x slower than this on cin, unless
// ameliorated by std::ios::sync_with_stdio(false) for g++, at least as
// of 6.3.0;  the trick does not help clang, at least as of 8.0.0)
// Mark Moraes, D. E. Shaw Research

#pragma once
#include <iostream>
#include <core123/strutils.hpp>
#include <core123/diag.hpp>
#include <core123/sew.hpp>

namespace core123 {

const size_t LBSIZE = 16384;
const char ENDCHAR = '\n';

// The actual processlines function takes a fillbuf lambda

inline void processlines(std::function<size_t(char *,size_t)>fillbuf,
                                std::function<void(const str_view)> f,
                                bool eofshortread = true,
                                size_t bufsize = LBSIZE,
                                char delim = ENDCHAR){
    auto _processlines = diag_name("processlines");
    std::unique_ptr<char[]> bufup(new char[bufsize]);
    auto bufstart = bufup.get();
    size_t buflen = 0;
    bool got_eof = false;
    DIAG(_processlines, "buf=" << (unsigned long long)bufup.get() << " size=" << bufsize << " len=" << buflen);
    while (!got_eof) {
        DIAG(_processlines, "bufstart=" << (unsigned long long)bufstart << " len=" << buflen);
        auto cp = static_cast<char *>(::memchr(bufstart, delim, buflen));
        size_t cpincr;
        DIAG(_processlines, "cp=" << (unsigned long long)cp);
        if (cp == nullptr) {
            if (bufstart > bufup.get()) {
                DIAG(_processlines, "moving " << buflen << " bytes");
                ::memmove(bufup.get(), bufstart, buflen);
                bufstart = bufup.get();
            }
            if (buflen == bufsize)
                throw std::runtime_error("out of buffer space, line too long?! buflen="+std::to_string(buflen)+" bufsize="+std::to_string(bufsize));
            auto nb = fillbuf(bufstart+buflen, bufsize-buflen);
            DIAG(_processlines, "filled " << nb << " bytes, eofshortread=" << eofshortread);
            if (nb < 0 || (nb == 0 && eofshortread)) {
                DIAG(_processlines, "EOF buflen=" << buflen);
                got_eof = true;
                if (buflen == 0)
                    break;
            } else {
                // either got more data or need to spin waiting for more
                buflen += nb;
                continue;
            }
            DIAG(_processlines, "bufstart=" << (unsigned long long)bufstart << " len=" << buflen);
            // got EOF or no data, and last line has no delim, so
            // we mimic std::getline and return it
            cp = bufstart + buflen; 
            cpincr = buflen;            // so we end loop after processing this line
        } else {
            cpincr = cp - bufstart + 1;        // so we skip the delim
        }
        str_view sv{bufstart, (size_t)(cp - bufstart)};
        DIAG(_processlines, "calling f with " << sv.size() << ": \"" << sv << "\" cpincr=" << cpincr);
        f(sv);
        bufstart += cpincr;
        buflen -= cpincr;
    }
}

// make lambda for istream

inline auto fillbuf_(std::istream& inp) {
    auto _processlines = diag_name("processlines");
    inp.exceptions(std::istream::badbit);
    return [_processlines, &inp](char *buf, size_t bufsize) {
        DIAG(_processlines, "reading " << bufsize << " bytes from stream");
        inp.read(buf, bufsize);
        ssize_t nb = inp.gcount();
        bool got_eof = inp.eof();
        bool got_fail = inp.fail();
        DIAG(_processlines, "read " << nb << " eof=" << got_eof << " fail=" << got_fail);
        if (!got_eof && got_fail)
            throw std::runtime_error("read from stream failed");
        return nb;
    };
}

// make lambda for file descriptor fd, relies on ::read()

inline auto fillbuf_(int fd) {
    auto _processlines = diag_name("processlines");
    return [_processlines, fd](char *buf, size_t bufsize) {
        DIAG(_processlines, "reading " << bufsize << " bytes from fd");
        size_t nb = sew::read(fd, buf, bufsize);
        DIAG(_processlines, "read " << nb);
        return nb;
    };
}

// make lambda for stdio FILE *, relies on ::fread()

inline auto fillbuf_(FILE *fp) {
    auto _processlines = diag_name("processlines");
    return [_processlines, fp](char *buf, size_t bufsize) {
        DIAG(_processlines, "reading " << bufsize << " bytes from fp");
        auto nb = sew::fread(buf, 1u, bufsize, fp);
        DIAG(_processlines, "read " << nb);
        return nb;
    };
}

template<typename T>inline void processlines(T& inp,
                         std::function<void(const str_view)> f,
                         bool eofshortread = true,
                         size_t bufsize = LBSIZE,
                         char delim = ENDCHAR) {
    processlines(fillbuf_(inp), f, eofshortread, bufsize, delim);
}

} // namespace core123

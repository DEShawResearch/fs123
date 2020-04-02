// Class to read from an libevent evbuffer via a stream.
/*
 * Adapted from fdstream.hpp
 * (C) Copyright Nicolai M. Josuttis 2001.
 * Permission to copy, use, modify, sell and distribute this software
 * is granted provided this copyright notice appears in all copies.
 * This software is provided "as is" without express or implied
 * warranty, and with no claim as to its suitability for any purpose.
 */

#pragma once

#include <istream>
#include <ostream>
#include <streambuf>
#include <cstdio> // for EOF
#include <cstring> // for memmove()
#include <event2/buffer.h> // for libevent evbuffer

namespace core123 {


// evistream: interface libevent evbuffer to stream

class evinbuf : public std::streambuf {
  protected:
    struct evbuffer *evbufp; // should this be unique_ptr?
  protected:
    /* data buffer:
     * - at most, pbSize characters in putback area plus
     * - at most, bufSize characters in ordinary read buffer
     */
    static const int pbSize = 4;        // size of putback area
    static const int bufSize = 1024;    // size of the data buffer
    char buffer[bufSize+pbSize];        // data buffer

  public:
    /* constructor
     * - initialize file descriptor
     * - initialize empty data buffer
     * - no putback area
     * => force underflow()
     */
    evinbuf (struct evbuffer *_ep) : evbufp(_ep) {
        setg (buffer+pbSize,     // beginning of putback area
              buffer+pbSize,     // read position
              buffer+pbSize);    // end position
    }

  protected:
    // insert new characters into the buffer
    virtual int_type underflow () {
#ifndef _MSC_VER
        using std::memmove;
#endif

        // is read position before end of buffer?
        if (gptr() < egptr()) {
            return traits_type::to_int_type(*gptr());
        }

        /* process size of putback area
         * - use number of characters read
         * - but at most size of putback area
         */
        int numPutback;
        numPutback = gptr() - eback();
        if (numPutback > pbSize) {
            numPutback = pbSize;
        }

        /* copy up to pbSize characters previously read into
         * the putback area
         */
        memmove (buffer+(pbSize-numPutback), gptr()-numPutback,
                numPutback);

        // read at most bufSize new characters
        int num;
        num = evbuffer_remove(evbufp, buffer+pbSize, bufSize);
        if (num <= 0) {
            // ERROR or EOF
            return EOF;
        }

        // reset buffer pointers
        setg (buffer+(pbSize-numPutback),   // beginning of putback area
              buffer+pbSize,                // read position
              buffer+pbSize+num);           // end of buffer

        // return next character
        return traits_type::to_int_type(*gptr());
    }
};

class evistream : public std::istream {
  protected:
    evinbuf buf;
  public:
    evistream (struct evbuffer *evbufp) : std::istream(0), buf(evbufp) {
        rdbuf(&buf);
    }
};

}  // core123 namespace

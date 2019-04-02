#ifndef __crfio_dot_hpp_
#define __crfio_dot_hpp_

// Utilities for reading and writing "CDB Record Format" records.

#include <string>
#include <cstdio>
#include <stdexcept>

namespace crfio{

// Write x.data() to fp.  Throw an xerror if any errors are
// detected.  The state of fp is undefined after an error.
void xwrite(const std::string& x, FILE *fp);

// call x.resize(len) and then read len bytes from fp
// into x.  Throw an xerror if a read error occurs.
// The state of the input and the contents of x are
// undefined after an error.
void xread(std::string& x, size_t len, FILE *fp);

// read x.size() bytes from fp.  Throw an xerror if the bytes read
// do not exactly match the contents of x.  The state of the input is
// undefined after an error.
void xpect(const std::string& x, FILE *fp);

// Write k and d to out in the serialized CDB Record Format.
// Throw an xerror if any errors occur.  The state of the
// output is undefined after an error.
void out(FILE *out, const std::string& k, const std::string& d);

// Write a newline to out, indicating the final record in the
// CDB Record Format.   Then flush the output file.  Throw
// an xerror if an error occurs.  The state of the output
// is undefined after an error.
void outeof(FILE *out);

// Read a character from in.  If it is a newline, return false,
// leaving k and d unchanged.  If it is a '+', then continue reading.
// If the input contains a serialized CDB Record Format record, then
// consume the input through the record's newline terminator,
// transfer the record to k and d, and return true.  Otherwise, throw
// an xerror. The contents of k and d, and the state of
// the input are undefined after an error.
bool in(FILE *in, std::string& k, std::string& d);

} // namespace crfio

#endif

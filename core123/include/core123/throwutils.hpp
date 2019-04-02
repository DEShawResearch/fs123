#pragma once

#include <core123/strutils.hpp>
#include <system_error>
#include <string>
#include <errno.h>

namespace core123 {

// se - it requires way too much typing to throw a std::system error
//   using std:: tools only.  The  se functions allow you to say:
//
//     throw core123::se("something didn't work");
//
//  It's especially useful if you're already using the core123
//  namespace and have #included strutils.hpp.  E.g.,
//
//     throw se(ESTALE, str(name, "doesn't look fresh i=", i, "j=", j));
//     

inline std::system_error se(int eno, const std::string& msg){
    return std::system_error(eno, std::system_category(), msg);
}

inline std::system_error se(const std::string& msg){
    return se(errno, msg);
}

// strfunargs - It's very handy to be able to quickly cobble together a
// call signature for a 'what' string.  strfunargs returns a string
// that "looks like" a function call.  For example, if b_t and
// c_t are OutputFormatable:
//
// some_func(int a, b_t b, c_t c){
//    ...
//    if(bad)
//       throw se(EFOO, strfunargs(__func__, a, b, c)) + ": that's bad")
//    ...
// }
//
// You might see a system-error with a 'what' string that looks like:
//
//      some_func(1, bbbbb, ccccc): that's bad
//
// There's nothing magic - it's just a call to str_sep, and a couple
// of std::string operator+'s and constant strings.

template <typename ... Args>
std::string
strfunargs(const std::string& name, Args ... args){
    return name + "(" + str_sep(", ", args...) + ")";
}
 
} // namespace core123

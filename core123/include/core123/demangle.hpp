#include <string>
#if __has_include(<cxxabi.h>)
#include <cxxabi.h>
#include "core123/autoclosers.hpp"
#include "core123/throwutils.hpp"
#endif

// It's just way too hard to call __cxa_demangle...  Let's make it
// easier.

namespace core123{
inline std::string demangle(const char *name){
#if __has_include(<cxxabi.h>)    
    int status;
    auto p = make_autocloser(abi::__cxa_demangle(name, nullptr, 0, &status), ::free);
    switch(status){
    case  0: return {p.get()};
    case -1: throw se(ENOMEM, strfunargs("demangle", name));
    case -2: throw se(EINVAL, strfunargs("demangle", name)); // ??? Too harsh?  Maybe just return name here?  
    default: throw se(EINVAL, strfunargs("__cxa_demangle", name) + " unexepected status");
    }
#else
    return name;
#endif
}

inline std::string demangle(const std::type_info& ti){
    return demangle(ti.name());
}
}

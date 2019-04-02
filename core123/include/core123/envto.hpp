#pragma once
#include <string>
#include <cstdlib>
#include <core123/svto.hpp>
#include <core123/throwutils.hpp>

namespace core123 {

// envto - Convert the named environment variable into a T.
// With two arguments, return the second arg (default)
// if the environment variable doesn't exist.
template <typename T>
T envto(const char *name, const T& dflt){
    const char *e=::getenv(name);
    if(e)
        return svto<T>(e);
    else
        return dflt;
}

template <typename T>
T envto(const char *name){
    const char *e=::getenv(name);
    if(e)
        return svto<T>(std::string(e));
    else
        throw core123::se(EINVAL, core123::strfunargs("envto", name));
}

// specialize envto<std::string> to return the whole string.
// DO NOT stop at the first whitespace.
template<>
inline std::string envto<std::string>(const char *name){
    const char *e=::getenv(name);
    if(e)
        return std::string(e);
    else
        throw core123::se(EINVAL, core123::strfunargs("envto", name));
}

template<>
inline std::string envto<std::string>(const char *name, const std::string& dflt){
    const char *e=::getenv(name);
    if(e)
        return std::string(e);
    else
        return dflt;
}

} // namespace core123

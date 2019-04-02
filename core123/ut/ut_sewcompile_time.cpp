#if defined(WITH_SEW)
#include "core123/system_error_wrapper.hpp"
namespace sew = system_error_wrapper;
#else
#include <unistd.h>
#include <stdexcept>
#include <memory>
#include <string>
#include <sstream>
#include <exception>
#include <system_error>
namespace sew{
int getpid(){
    int ret = ::getpid();
    if(ret < 0)
        throw std::runtime_error("getpid failed");
    return ret;
}
}
#endif

int main(int, char **){
    return !sew::getpid();
}

#include "selector_manager.hpp"
#include <core123/sew.hpp>
#include <core123/exnest.hpp>

using namespace core123;

// validate_basepath - throw an exception if the path "looks bad".
//   Now that we use chroot, this is far less crucial as
//   a "security" measure.
void
per_selector::validate_basepath(const std::string& bp) try {
    if(bp.empty())
        throw se(EINVAL, "basepath may not be empty");
    if(bp.find('\0') != std::string::npos)
        throw se(EINVAL, "basepath may not contain NUL");
    // FIXME - If we're going to bother to open it, why don't we
    // keep it open and use it with 'at' functions in the handler?
    sew::close(sew::open(bp.c_str(), O_DIRECTORY));
 }catch(std::exception& e){
    std::throw_with_nested( std::runtime_error("validate_basepath(" + bp + ")"));
 }

// validate_estale_cookie - throw an exception if the value
//   isn't one of the known/accepted values.  (FIXME - does
//   this really belong here).
void
per_selector::validate_estale_cookie(const std::string& value){
    if(!(
         value == "ioc_getversion" ||
         value == "st_ino" ||
         value == "setxattr" ||
         value == "getxattr" ||
         value == "none"))
        throw se(EINVAL, "estale_cookie_src must be one of ioc_getversion|st_ino|setxattr|getxattr|none");
}


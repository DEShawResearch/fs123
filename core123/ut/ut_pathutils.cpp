#include "core123/pathutils.hpp"
#include "core123/sew.hpp"
#include "core123/autoclosers.hpp"
#include "core123/ut.hpp"
#include <iostream>

namespace sew = core123::sew;
namespace ac = core123::ac;

void test_makedirs(){
    // N.B.  If this fails, use strace to see what it's doing,
    // which usually makes the error obvious.

    // First, let's check a couple of pathological calls..
    try {
        core123::makedirs("///", 0777);
    } catch (std::system_error& xe) {
        EQUAL (xe.code().value(), EEXIST);
    }
    core123::makedirs("///", 0777, true);

    try {
        core123::makedirs("", 0777);
    } catch (std::system_error& xe) {
        // According to POSIX, mkdir("") sets errno to ENOENT
        EQUAL(xe.code().value(), ENOENT);
    } 

    char name[] = "/tmp/test_makedirsXXXXXX";
    sew::mkdtemp(name);
    std::string name_s(name);
    core123::makedirs(name_s + "///abc", 0777);
    try {
        core123::makedirs(name_s + "/abc/", 0777);
        CHECK(false);
    } catch (std::system_error& xe) {
        EQUAL (xe.code().value(), EEXIST);
    }
    core123::makedirs(name_s + "/abc/", 0777,  true);

    core123::makedirs(name_s + "/abc/def", 0777);
    core123::makedirs(name_s + "/abc/hij/klm/nop", 0777);
    // Make some with relative paths
    char cwd[PATH_MAX+1];
    sew::getcwd(cwd, sizeof(cwd));
    sew::chdir(name);
    core123::makedirs("xyz/uvw", 0777);
    core123::makedirs("xyz///mnop//pqr/", 0777);
    sew::chdir(cwd);
    
    // check that we really do get an EEXIST when a path component
    // is not a directory, even with exist_ok=true
    sew::close(sew::open((name_s + "/xyz///mnop//pqr/file").c_str(), O_CREAT|O_WRONLY, 0666));
    try{
        core123::makedirs(name_s + "/xyz///mnop//pqr/file", true);
        CHECK(false);
    }catch(std::system_error& xe){
        EQUAL (xe.code().value(), EEXIST);
    }
    try{
        core123::makedirs(name_s + "/xyz///mnop//pqr/file/wontwork", true);
        CHECK(false);
    }catch(std::system_error& xe){
        EQUAL (xe.code().value(), ENOTDIR);
    }
    sew::unlink((name_s + "/xyz///mnop//pqr/file").c_str());

    core123::sew::chmod((name_s + "/abc/hij").c_str(), 0500);
    try {
        core123::makedirs(name_s + "/abc//hij/xyzz/", 0777);
        CHECK(geteuid() == 0); // root doesn't get EACCES
        sew::rmdir((name_s + "/abc//hij/xyzz/").c_str());
    } catch (std::system_error& xe) {
        EQUAL (xe.code().value(), EACCES);
    }
    // Clean up.  Also checks that directories were created.
    sew::rmdir((name_s + "/xyz/mnop/pqr").c_str());
    sew::rmdir((name_s + "/xyz/mnop").c_str());
    sew::rmdir((name_s + "/xyz/uvw").c_str());
    sew::rmdir((name_s + "/xyz").c_str());
    sew::rmdir((name_s + "/abc/hij/klm/nop").c_str());
    sew::chmod((name_s + "/abc/hij").c_str(), 0700);
    sew::rmdir((name_s + "/abc/hij/klm").c_str());
    sew::rmdir((name_s + "/abc/hij").c_str());
    sew::rmdir((name_s + "/abc/def").c_str());
    sew::rmdir((name_s + "/abc").c_str());

    //
    // Now do it all again with makedirsat...
    //
    ac::fd_t<> namefd = sew::open(name, O_DIRECTORY);
    core123::makedirsat(namefd, "abc", 0777);
    try {
        core123::makedirsat(namefd, "abc/", 0777);
        CHECK(false);
    } catch (std::system_error& xe) {
        EQUAL (xe.code().value(), EEXIST);
    }
    core123::makedirsat(namefd, "abc/", 0777,  true);
    
    core123::makedirsat(namefd, "abc/def", 0777);
    core123::makedirsat(namefd, "abc/hij/klm/nop", 0777);
    // Make some with relative paths
    sew::getcwd(cwd, sizeof(cwd));
    sew::chdir(name);
    core123::makedirsat(AT_FDCWD, "xyz/uvw", 0777);
    core123::makedirsat(AT_FDCWD, "xyz///mnop//pqr/", 0777);
    sew::chdir(cwd);
    
    // check that we really do get an EEXIST when a path component
    // is not a directory, even with exist_ok=true
    sew::close(sew::openat(namefd, "xyz///mnop//pqr/file", O_CREAT|O_WRONLY, 0666));
    try{
        core123::makedirsat(namefd, "xyz///mnop//pqr/file", true);
        CHECK(false);
    }catch(std::system_error& xe){
        EQUAL (xe.code().value(), EEXIST);
    }
    try{
        core123::makedirsat(namefd, "xyz///mnop//pqr/file/wontwork", true);
        CHECK(false);
    }catch(std::system_error& xe){
        EQUAL (xe.code().value(), ENOTDIR);
    }
    sew::unlinkat(namefd, "xyz///mnop//pqr/file", 0);

    core123::sew::fchmodat(namefd, "abc/hij", 0500, 0);
    try {
        core123::makedirsat(namefd, "abc//hij/xyzz/", 0777);
        CHECK(geteuid() == 0); // root doesn't get EACCES
        sew::unlinkat(namefd, "abc//hij/xyzz/", 0);
    } catch (std::system_error& xe) {
        EQUAL (xe.code().value(), EACCES);
    }
    // Clean up.  Also checks that directories were created.
    sew::unlinkat(namefd, "xyz/mnop/pqr", AT_REMOVEDIR);
    sew::unlinkat(namefd, "xyz/mnop", AT_REMOVEDIR);
    sew::unlinkat(namefd, "xyz/uvw", AT_REMOVEDIR);
    sew::unlinkat(namefd, "xyz", AT_REMOVEDIR);
    sew::unlinkat(namefd, "abc/hij/klm/nop", AT_REMOVEDIR);
    sew::fchmodat(namefd, "abc/hij", 0700, 0);
    sew::unlinkat(namefd, "abc/hij/klm", AT_REMOVEDIR);
    sew::unlinkat(namefd, "abc/hij", AT_REMOVEDIR);
    sew::unlinkat(namefd, "abc/def", AT_REMOVEDIR);
    sew::unlinkat(namefd, "abc", AT_REMOVEDIR);
    
    sew::rmdir(name);
}

int main(int, char **){
    test_makedirs();
    return utstatus();
}

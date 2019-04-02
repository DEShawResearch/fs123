#include "core123/pathutils.hpp"
#include "core123/sew.hpp"
#include "core123/ut.hpp"
#include <iostream>

namespace sew = core123::sew;

void test_makedirs(){
    // N.B.  If this fails, use strace to see what it's doing,
    // which usually makes the error obvious.

    // First, let's check a couple of pathological calls..
    try {
        core123::makedirs("///", 0777);
    } catch (std::system_error& xe) {
        EQUAL (xe.code().value(), EEXIST);
    }
    try {
        core123::makedirs("", 0777);
    } catch (std::system_error& xe) {
        EQUAL(xe.code().value(), EEXIST);
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
    core123::makedirs(name_s + "/abc/def", 0777);
    core123::makedirs(name_s + "/abc/hij/klm/nop", 0777);
    // Make some with relative paths
    char cwd[PATH_MAX+1];
    sew::getcwd(cwd, sizeof(cwd));
    sew::chdir(name);
    core123::makedirs("xyz/uvw", 0777);
    core123::makedirs("xyz///mnop//pqr/", 0777);
    sew::chdir(cwd);
    
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
    sew::rmdir(name);
}

int main(int, char **){
    test_makedirs();
    return utstatus();
}

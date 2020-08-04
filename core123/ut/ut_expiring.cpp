#include "core123/expiring.hpp"
#include "core123/threeroe.hpp"
#include <cassert>
#include <unistd.h>
#include <iostream>

using std::cout;
using core123::expiring;
using core123::expiring_cache;
using core123::threeroe;

struct Int{
    int i;
    Int(int _i=0): i{_i}{}
    operator int() const{ return i; }
};

int scramble(int i){
    return threeroe(&i, sizeof(i)).hashpair64().first;
}

int main(int argc, char **argv){
    // An early implementation of evict (called by insert)
    // had a bad bug that potentially segfaulted.  I can't
    // reliably get it to segfault in a unit test, but I *can*
    // reliably get valgrind to complain.  So we run the
    // unit test under valgrind
#ifndef __APPLE__
    if( argc <= 1 && !getenv("NO_VALGRIND") ){
        std::cout << argv[0] << " called with no arguments reinvokes itself under valgrind" << std::endl;
        // Using valgrind with tcmalloc is terribly finnicky.  We
        // assume that the build system placed things like
        // --suppressions= and --soname-synonyms= in the pp-symbol
        // VALGRIND_OPTS (with all the necessary punctuation).
#ifdef VALGRIND_OPTS
        const char *args[] = {"valgrind", VALGRIND_OPTS, "--error-exitcode=1", argv[0], "under_valgrind", nullptr};
#else
        const char *args[] = {"valgrind", "--error-exitcode=1", argv[0], "under_valgrind", nullptr};
#endif
        execvp( "valgrind", const_cast<char * const *>(args) );
        std::cout << "Uh oh.  execvp returned???\n";
        return 2; // can
    }
#endif // __APPLE__

    expiring_cache<int, Int> ec(100);

    for(int i=0; i<50; ++i)
        ec.insert(scramble(i), -i, std::chrono::milliseconds(100));
    assert(ec.evictions() == 0);
    for(int i=0; i<50; ++i)
        assert(ec.lookup(scramble(i)) == -i);
    for(int i=0; i<50; ++i)
        assert(!ec.lookup(scramble(i)).expired() && ec.lookup(scramble(i)) == -i);
    cout << "OK - lookups pass\n";

    ::sleep(1);
    for(int i=0; i<50; ++i)
        assert(ec.lookup(scramble(i)).expired()); // all should have expired in  1 sec.
    assert(ec.size() == 0);
    cout << "OK - lookups fail after timeout expires\n";

    for(int i=50; i<2500; ++i)
        ec.insert(scramble(i), -i, std::chrono::milliseconds(100));
    assert(ec.size() <= 100 && ec.size() > 96);
    cout << "OK - size was limited to 100 entries\n";
    
    size_t nfound = 0;
    for(int i=50; i<2500; ++i){
        auto x = ec.lookup(scramble(i));
        if( !x.expired() ){
            assert(x == -i);
            ++nfound;
        }
    }
    assert(nfound==ec.size());
    cout << "OK - found " << nfound << " insertions\n";

    ::sleep(1);
    ec.erase_expired();
    assert(ec.size()==0);
    cout << "OK - nothing left after sleep(1) followed by erase_expired()\n";

    return 0;
}

#include <core123/circular_shared_buffer.hpp>
#include <core123/sew.hpp>
#include <core123/ut.hpp>
#include <thread>
#include <chrono>

using core123::circular_shared_buffer;
namespace sew = core123::sew;

// Create a fairly small circlogbuf, and then fork two threads.  One
// aggressively writes into it and the other tries to copy records
// out of it.  Check that the 'valid' records have been copied intact,
// and that not-too-many invalids were returned.

int main(int, char **){
    auto childpid = sew::fork();
    static size_t HOWMANY=1000000;
    static const int record_sz = circular_shared_buffer::record_sz;
    if(childpid){
        // parent - writer
        circular_shared_buffer foo("foo", O_RDWR|O_CREAT|O_TRUNC, getpagesize()/circular_shared_buffer::_block_sz);
        for(size_t i=0; i<HOWMANY; ++i){
            auto p = foo.ac_record();
            ::memset(p->data(), 'a'+i%26, p->size());
        }
        int status;
        sew::waitpid(childpid, &status, 0);
        if(WIFEXITED(status))
            return WEXITSTATUS(status);
        else
            return 99;
    }else{
        // child - reader
        circular_shared_buffer foo("foo", O_RDONLY);
        // Give the parent thread time to get started...
        bool started = false;
        for(int w=0; w<100; ++w){
            char dest[record_sz];
            if(foo.copyrecord(0, dest)){
                started = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(started);
        if(!started){
            printf("Child sees nothing.  Did the parent start cleanly?\n");
            return 1;
        }
        
        size_t invalid = 0;
        for(size_t i=0; i<HOWMANY; ++i){
            char dest[record_sz];
            bool ok = foo.copyrecord(i, dest);
            if(ok){
                char d0 = dest[0];
                for(int j=1; j<record_sz; ++j){
                    if(dest[j] != d0){
                        printf("Oops.  torn record %zd: %.*s\n", i, record_sz, dest); 
                        ok = false;
                        break;
                    }
                }
                CHECK(ok);
            }else{
                invalid++;
                if(invalid < 100)
                    printf("Detected invalid record %zd: %.*s\n", i, record_sz, dest);
            }
        }
        float badfrac = (float)invalid/HOWMANY;
        printf("%zu invalid records found in %zu attempts:  %.2f%% invalid\n", invalid, HOWMANY, badfrac*100.);
        if( badfrac > 0.02 )
            printf("More than 2%% invalid is a lot.  Run it again.  Is there something sketchy about this filesystem?\n");
        CHECK(badfrac < 0.1); // fail if more than 10% are bad.  That's a lot worse than we typically see.
        return utstatus(1);
    }
    return 98; // not reached
}

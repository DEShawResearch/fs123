#include <core123/circular_shared_buffer.hpp>
#include <core123/sew.hpp>
#include <core123/ut.hpp>
#include <core123/envto.hpp>
#include <thread>
#include <chrono>

using core123::circular_shared_buffer;
using core123::envto;
namespace sew = core123::sew;
using std::string;

string tmpfilename;
bool xattrs_working;

void setup(){
    string tmpdir = envto<string>("TMPDIR", "/tmp");
    tmpfilename = tmpdir + "/ut_circular_shared_buffer.XXXXXX";
    // Ignore the fd returned by mkstemp.  We're using it for
    // convenience, not security.  In fact, we're going to
    // create and remove this file multiple times, and accept
    // the fact that this exposes us to the risk of somebody
    // else replacing it behind our back.
    sew::mkstemp(&tmpfilename[0]);
    std::cout << "Temporary name: " << tmpfilename << "\n";
    // does the TMPDIR support xattrs?
#ifndef __APPLE__
    auto ret = setxattr(&tmpfilename[0], "user.testing", "ok", 2, 0);
#else
    auto ret = setxattr(&tmpfilename[0], "user.testing", "ok", 2, 0, 0);
#endif
    xattrs_working = (ret == 0);
    sew::unlink(&tmpfilename[0]);
}

void single_process_tests(){
    // Try to exercise various paths through the constructor.
    // The fact that this is so complicated suggests a mis-design,
    // but here we are...
    const size_t pgsz = ::getpagesize();
    const size_t def_rec_sz = circular_shared_buffer::default_record_sz;
    const size_t cksum_sz = circular_shared_buffer::cksum_sz;
    std::unique_ptr<circular_shared_buffer> csb1;
    std::unique_ptr<circular_shared_buffer> csb2;
    // default record size - super-easy.
    csb1 = std::make_unique<circular_shared_buffer>(tmpfilename, O_RDWR|O_CREAT, pgsz/def_rec_sz);
    EQUAL(csb1->record_sz, def_rec_sz);
    EQUAL(csb1->N, pgsz/def_rec_sz);
    csb2 = std::make_unique<circular_shared_buffer>(tmpfilename, O_RDWR);
    EQUAL(csb2->N, csb1->N);
    csb1.reset(); csb2.reset(); sew::unlink(&tmpfilename[0]);
    
    // non-default record size
    size_t my_recsz = 95;
    csb1 = std::make_unique<circular_shared_buffer>(tmpfilename, O_RDWR|O_CREAT, pgsz, my_recsz);
    EQUAL(csb1->record_sz, my_recsz);
    EQUAL(csb1->N, pgsz);
    if(xattrs_working){
        csb2 = std::make_unique<circular_shared_buffer>(tmpfilename, O_RDWR);
    }else{
        // if there are no xattrs, we have to "know" the recordsz to read:
        csb2 = std::make_unique<circular_shared_buffer>(tmpfilename, O_RDONLY, 0, my_recsz);
    }
    EQUAL(csb2->N, csb1->N);

    auto p = csb1->ac_record();
    p.close();  // !!! Must close before we reassign to csb1 !!!
    auto blob = csb2->getblob(0);
    CHECK(blob);

    // re-open without CREAT, but for writing
    if(xattrs_working){
        csb1 = std::make_unique<circular_shared_buffer>(tmpfilename, O_RDWR);
    }else{
        // if there are no xattrs, we have to "know" the recordsz to read:
        csb1 = std::make_unique<circular_shared_buffer>(tmpfilename, O_RDWR, 0, my_recsz);
    }
    EQUAL(csb1->N, csb2->N); // same number of records

    // Let's wrap around at least once
    for(size_t i=0; i<csb1->N + 100; ++i){
        p = csb1->ac_record(); // exercise move-assignment of ac_records
        EQUAL(p->size(), my_recsz - cksum_sz);
        memset(p->data(), (i%26) + 'a' , p->size());
        // Try to 'get' this blob while it's still being written
        blob = csb2->getblob(i);
        CHECK(!blob);  // i.e., it should be invalid
        if(i>0){
            // but the previous one should be closed and valid by now
            blob = csb2->getblob(i-1);
            CHECK(bool(blob));
        }
    }
    p.close(); // close the last one!
    // And check that they all still look good
    for(size_t i=100; i<csb1->N+100; ++i){
        blob = csb2->getblob(i);
        CHECK(blob);
        string yyy((i%26) + 'a', blob.size());
        CHECK(::memcmp(blob.data(), yyy.data(), blob.size()));
    }
    
    ::unlink(&tmpfilename[0]);
}

void test_read_write(){
    // Create a fairly small circlogbuf, and then fork two threads.  One
    // aggressively writes into it and the other tries to copy records
    // out of it.  The child check that the 'valid' records have been copied intact,
    // and that not-too-many invalids were returned.  The parent waits
    // and checks that the child's exit status is zero.
    static size_t HOWMANY=1000000;
    circular_shared_buffer writer(tmpfilename, O_RDWR|O_CREAT|O_TRUNC, 32);
    auto childpid = sew::fork();
    if(childpid == 0){
        // child - reader
        circular_shared_buffer reader(tmpfilename, O_RDONLY);
        bool started = false;
        for(int w=0; w<100; ++w){
            if(reader.getblob(0)){
                started = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(started);
        if(!started){
            printf("Child sees nothing.  Did the parent start cleanly?\n");
            return;
        }
        
        size_t invalid = 0;
        for(size_t i=0; i<HOWMANY; ++i){
            core123::uchar_blob b = reader.getblob(i);
            // b has data() and size() members and a bool conversion:
            if(b){
                bool ok = true;
                auto dest = (char *)b.data();
                int dsz = b.size();
                auto d0 = dest[0];
                for(int j=1; j<dsz; ++j){
                    if(dest[j] != d0){
                        printf("Oops.  torn record %zd: %.*s\n", i, dsz, dest); 
                        ok = false;
                        break;
                    }
                }
                CHECK(ok);
            }else{
                invalid++;
                if(invalid < 100)
                    printf("Detected invalid record %zd\n", i);
            }
        }
        float badfrac = (float)invalid/HOWMANY;
        printf("%zu invalid records found in %zu attempts:  %.2f%% invalid\n", invalid, HOWMANY, badfrac*100.);
        if( badfrac > 0.02 )
            printf("More than 2%% invalid is a lot.  Run it again.  Is there something sketchy about this filesystem?\n");
        CHECK(badfrac < 0.3); // fail if more than 30% are bad.  That's a lot worse than we typically see.
        exit(utstatus(1));
    }
    // parent - writer
    for(size_t i=0; i<HOWMANY; ++i){
        auto p = writer.ac_record();
        ::memset(p->data(), 'a'+i%26, p->size());
    }
    int status;
    sew::waitpid(childpid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    sew::unlink(&tmpfilename[0]);
}

int main(int, char **){
    setup();
    single_process_tests();
    test_read_write();
    return utstatus(1);
}

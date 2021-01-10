#pragma once
#include <core123/sew.hpp>
#include <core123/threeroe.hpp>
#include <core123/autoclosers.hpp>
#include <core123/uchar_span.hpp>
#include <core123/svto.hpp>
#include <atomic>
#include <cstring>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <unistd.h>
#include <sys/mman.h>

// circular_shared_buffer - a shared-memory, file-backed, thread-safe
//   circular buffer, very loosely inspired by Varnish's log system.
//
//  The constructor has two required and three optional arguments:
//   circular_shared_buffer(filename, oflags, N=0, recordsz=0, mode=0600)

//  Both oflag and mode are passed to open and have their usual
//  meaning.
//
//  If O_TRUNC is set in oflags, then the file's previous content, if
//  any, is discarded and it is reinitialized with N invalid records.
//  In this case, N must explicitly provided.
//
//  If O_CREAT is set in oflags, and if the file did not previously
//  exist, then it is initialized with N invalid records (which must
//  be explicitly provided).  If the file did exist, and O_TRUNC is
//  not set, then the previous contents is left intact and the number
//  of records is determined from the file's size.  N is ignored.
//
//  If neither O_TRUNC nor O_CREAT is set in oflags, then the file
//  must exist, and the number of records is determined from the file's
//  size.  N is ignored.
//   
//  Also note that it's possible for the constructor to throw after
//  filename is opened.  If oflags contains O_TRUNC or O_CREAT, the
//  visible state of the filesystem may be changed.  Consequently,
//  the constructor has "basic" but not "strong"  exception safety.
//
//  Each record contains record_sz bytes, of which cksum_sz (8) are
//  reserved for an automatically generated checksum.  The record_sz
//  is determined as follows:
//    -if the constructor argument is non-zero, then use it.
//    -else, if the file already exists and has an extended attribute
//     named user.circular_shared_buffer.record_sz, then use that.
//    -else, use the default_record_sz (128).
//  When the constuctor creates a new file, it tries to add an
//  extended attribute named user.circular_shared_buffer.record_sz,
//  but it is not an error if it is unable to do so.
//
//  The 'records' returned by ac_record (for writing) and getblob()
//  (for reading) have a usable size of record_sz-cksum_sz.  A file
//  with N records is of length N*record_sz, which must be a multiple
//  of the pagesize (4096).
//
//  If the file is opened for writing (O_RDWR, O_WRONLY is silently
//  promoted to O_RDWR), the program may call ac_record() which
//  returns an autocloser_t<span<unsigned char>>.  I.e., a smart-pointer to a
//  span<unsigned char> that 'finishes' the shared record when it is destroyed.
//  Usage is something like:
//
//    {
//      auto r = csb.ac_record();
//      snprintf(r->data(), r->size(), "...", ...);
//      // or
//      memcpy(r->data(), ???, r->size());
//    }  // the record is finalized when r goes out-of-scope
//      
//  A reading process accessing this record in the shared file will
//  see an 'invalid' record from the time ac_record() is called until
//  'r' goes out of scope (or its close() method is called).  The
//  record will be valid from then on - until the N'th subsequent
//  ac_record() call, at which time, the circular buffer wraps around
//  so the same memory is returned by the later call.  To make the
//  record valid as quickly as possible, define the value returned by
//  ac_record with a narrow scope (as above).
//
//  Note that 'r' must be close()-ed or destroyed before the
//  circular_shared_buffer from which it was obtained is destroyed.
//  Violation of this requirement will likely result in a segfault.
//  Defining the value returned by ac_record() with a narrow scope
//  will generally avoid this problem.
//
//  Multiple threads in a single process may write concurrently, but
//  it is an error if more than one process (or thread) opens a file
//  for writing.  Circular shared buffers are expected to be
//  memory-mapped into multiple processes.  Nothing is guaranteed if
//  readers and writers access the "same" file over a networked
//  filesystem.  Behavior is undefined (probably a segfault) if a
//  writer re-opens a file with O_TRUNC and a new value of N while a
//  reader has it open.
//
//  Readers should expect that another process may be actively writing
//  to the file while they are reading.  Data in the file is
//  ephemeral.  It may be overwritten at any time.  Readers must
//  copy records into their own memory before using the data.
// 
//  If the file is opened for reading (O_WRONLY not set), the program
//  can copy records from an circular_shared_buffer, csb, with:
//
//    unsigned char dest[csb.record_sz];
//    bool ok = csb.copyrecord(i, dest)
//
//  or
//    auto b = getblob(i);
//
//  The former requires the caller to manage storage for dest, which
//  *must* have space for b.record_sz bytes.  The latter returns a
//  core123::uchar_blob, which has RAII semantics, data() and size()
//  methods and can be automagically converted to bool or
//  tcb::span<unsigned char>.  Usage might be something like:
//
//     if(b)
//        printf("%.*s", b.size(), b.data());
//  
//  In either case, readers should ignore/discard invalid data,
//  recognizing that invalid data is "normal" and should not be
//  considered a program error.
//
//  Notes:
//
//  There is a low-level startrecord()/finishrecord API that works
//  with unsigned char* instead of span<unsigned char>, and that does
//  *not* autoclose.  The ac_record API is preferred.
//
//  Note that the contents of the records is completely at the
//  discretion of the writer.  Writers may (or may not) choose to put
//  data in the buffer (e.g., sequence numbers, timestamps) that allow
//  readers to deduce the order in which records were written, whether
//  they've "seen" a record before, whether records are part of a
//  single transaction, etc.
//
//  Also note that it's up to writers whether the data are "text" or
//  "binary".  The code uses printable characters (hex digits and
//  formfeeds) to validate and invalidate records, so if writers
//  choose to write only text, the entire file will be text, and may
//  even be manageable with shell-based text-processing tools (though
//  validity checking would be tricky).
//
//  Caveats:
//
//  Checksums are 32 bits (hex-encoded into 8 bytes), so collisions
//  are unlikely but not impossible.  Expect a false positive, i.e., a
//  record that is reported valid but in fact has been corrupted by a
//  concurrent writer approximately once for every 4 billion correctly
//  reported invalid records.
//
//  Future work:
//
//  Make the record size and checksum size runtime parameters.  The
//  only tricky part is to find someplace for the metadata that would
//  allow re-users to learn the record and checknum size of existing
//  files.
//

#include <core123/span.hpp>

namespace core123{

struct circular_shared_buffer{
    static constexpr size_t default_record_sz = 128;
    static constexpr size_t cksum_sz = 8;
    size_t record_sz;
    size_t data_sz;
    unsigned char *baseaddr;
    core123::ac::fd_t<> fd;
    size_t N;
    bool writable;
    std::atomic<uint64_t> recordctr = 0;

    circular_shared_buffer(const std::string& filename, int flags, size_t _N=0, size_t _record_sz = 0, int mode=0600)
    {
        fd = sew::open(filename.c_str(), flags, mode);
        if( flags & O_WRONLY ){
            // Opening with O_WRONLY is hopeless.  mmap with MAP_SHARED and PROT_WRITE requires
            // O_RDWR.  O_WRONLY is not enough (see the mmap man page).
            flags &= ~O_WRONLY;
            flags |= O_RDWR;
        }
        writable = (flags&O_RDWR);
        struct stat sb;
        sew::fstat(fd, &sb);
        size_t filesz = sb.st_size;
        bool just_created = (flags&O_CREAT) && (filesz == 0);
        int mmflags;
        if(_record_sz){
            record_sz = _record_sz;
        }else if(just_created){
            record_sz = default_record_sz;
        }else{
            char rszbuf[32];
            // Look for the record size in an xattr.  If there's no
            // such xattr, use default_record_sz.  A missing xattr is
            // *not* an error.
#ifndef __APPLE__
            auto rszlen = fgetxattr(fd, "user.circular_shared_buffer.record_sz", rszbuf, sizeof(rszbuf));
#else
            auto rszlen = fgetxattr(fd, "user.circular_shared_buffer.record_sz", rszbuf, sizeof(rszbuf), XATTR_NOFOLLOW);
#endif
            if(rszlen >= 0 && size_t(rszlen) <= sizeof(rszbuf)){
                record_sz = svto<size_t>(str_view{rszbuf, size_t(rszlen)});
            }else{
                record_sz = default_record_sz;
            }
        }            
        if(writable){
            bool do_truncate = (flags&O_TRUNC) || just_created;
            if(do_truncate){
                if(_N == 0)
                    throw std::runtime_error("circular_shared_buffer:  must specify a non-zero number of records");
                filesz = _N * record_sz;
                if(filesz%getpagesize())
                    throw std::runtime_error("circular_shared_buffer: file size must be divisible by pagesize");
                sew::ftruncate(fd, 0);
                sew::ftruncate(fd, filesz);
                std::string rsc = str(record_sz);
#ifndef __APPLE__
                // Ignore errors.  It's ok if the filesystem doesn't support xattrs.
                // We could complain, but what if the complaint log is a circular-buffer.
                // Let's just let this one go...
                (void)fsetxattr(fd, "user.circular_shared_buffer.record_sz", rsc.c_str(), rsc.size(), 0);
#else
                (void)fsetxattr(fd, "user.circular_shared_buffer.record_sz", rsc.c_str(), rsc.size(), 0, 0);
#endif
            }
            mmflags = PROT_WRITE|PROT_READ;
        }else{
            int retry = 0;
            while(filesz == 0 && retry++ < 5){
                // If a writer process is in the do_truncate branch, give it
                // a chance to set the file's size.
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                sew::fstat(fd, &sb);
                filesz = sb.st_size;
            }
            mmflags = PROT_READ;
        }
        if(filesz==0 || filesz%getpagesize() || filesz%record_sz)
            throw std::runtime_error("circular_shared_buffer: file size must be non-zero and divisible by pagesize");
        N = filesz / record_sz;
        if(record_sz < cksum_sz)
            throw std::runtime_error("circular_shared_buffer: record_sz must be at least cksum_sz (" + str(cksum_sz) + ")");
        data_sz = record_sz - cksum_sz;
        baseaddr = (unsigned char *)sew::mmap(nullptr, filesz, mmflags, MAP_SHARED, fd, 0);
    }            
        
    ~circular_shared_buffer(){
        // Don't use sew.  We can't throw.
        ::munmap(baseaddr, N*record_sz);
    }

    unsigned char *startrecord() {
        if(!writable)
            throw std::runtime_error("circular_shared_buffer: not writable");
        auto ret = baseaddr + (recordctr.fetch_add(1)%N)*record_sz;
        ::memset(&ret[data_sz], '\f', cksum_sz);
        std::atomic_thread_fence(std::memory_order_release);
        return ret;
    }
    
    void finishrecord(unsigned char *record) {
        if(!writable)
            throw std::runtime_error("circular_shared_buffer: not writable");
        printable_cksum(record, data_sz, &record[data_sz]);
        std::atomic_thread_fence(std::memory_order_release);
    }

#if 0
    // This *should* work, but
    //     https://gcc.gnu.org/bugzilla/show_bug.cgi?id=70832
    // makes the ac_record not-assignable because moving the lambda
    // tries to call the lambda's deleted *copy*-assignment instead of
    // its default *move*-assignment operator.
    auto ac_record(){
        using S=tcb::span<unsigned char>;
        return core123::make_autocloser(new S(startrecord(), data_sz),
                               [this](S *p){
                                   finishrecord(p->data());
                                   delete p;
                               });
    }
#else
    // lambdas are just syntactic sugar around a class with an operator()
private:
    struct record_closer{
        circular_shared_buffer* csb;
        record_closer(circular_shared_buffer *this_csb) : csb(this_csb){}
        void operator()(tcb::span<unsigned char>* p){
            csb->finishrecord(p->data());
            delete p;
        }
    };
public:
    auto ac_record(){
        using S=tcb::span<unsigned char>;
        return core123::make_autocloser(new S(startrecord(), data_sz),
                                        record_closer(this));
    }
#endif

    // printable_cksum writes a cksum_sz checksum each byte
    // of which is an ascii digit, to dest.  Think of it as a crc,
    // written out as hex digits.
    static void printable_cksum(void *p, size_t len, unsigned char* dest){
        // threeroe is overkill, but there's no crc32 in libc.
        uint64_t h = threeroe(p, len).hash64();
        for(size_t i=0; i<cksum_sz; ++i){
            dest[i] = "0123456789abcdef"[h&0xf];
            h >>= 4;
        }
    }

    bool copyrecord(size_t i, unsigned char *dest) const {
        auto record = baseaddr + record_sz*(i%N);
        unsigned char storedsum[cksum_sz];
        std::atomic_thread_fence(std::memory_order_acquire);
        ::memcpy(dest, record, data_sz);
        ::memcpy(storedsum, &record[data_sz], cksum_sz);
        unsigned char computedsum[cksum_sz];
        printable_cksum(dest, data_sz, computedsum);
        bool ret = !::memcmp(storedsum, computedsum, cksum_sz);
        return ret;
    }

    uchar_blob getblob(size_t i) const {
        uchar_blob ret(data_sz);
        if(copyrecord(i, ret.data()))
            return ret;
        return {};
    }

    size_t nrecords() const { return N; }
    bool is_writable() const { return writable; }

    // volatile_record_addr - Not recommended!  But it might be useful if
    // somebody wants to do their own polling, checksum-ing or
    // debugging.
    unsigned char *volatile_record_addr(size_t i) const{ return baseaddr + record_sz*(i%N); }
};
    
} // namespace core123

#pragma once
#include <core123/sew.hpp>
#include <core123/threeroe.hpp>
#include <core123/autoclosers.hpp>
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
//  The constructor has two required and two optional arguments:
//   circular_shared_buffer(filename, oflags, N=8192, mode=0600)

//  Both oflag and mode are passed to open and have their usual
//  meaning.
//
//  If O_TRUNC is set in oflags, then the file's previous content, if
//  any, is discarded and it is reinitialized with N invalid records.
//  The default value of N results in a 1MB file.
//
//  If O_CREAT is set in oflags, and if the file did not previously
//  exist, then it is initialized with N invalid records.  If the file
//  did exist, and O_TRUNC is not set, then the previous contents is
//  left intact and the number of records is determined from the
//  file's size.
//
//  If neither O_TRUNC nor O_CREAT is set in oflags, then the file
//  must exist, and the number of records is determined from the file's
//  size.
//   
//  Each record contains recordsz (120) bytes.  However, a file with N
//  records is of length N*_blksz (N*128), which must be a multiple of
//  the pagesize (4096).  I.e., N must be a multiple of 32.  The extra
//  bytes are used for per-record checksums.
//
//  If the file is opened for writing (O_WRONLY or O_RDWR), the
//  program may call ac_record() which returns an
//  autocloser_t<span<char>>.  I.e., a smart-pointer to a span<char>
//  that 'finishes' the shared record when it is destroyed.  Usage is
//  something like:
//
//    {
//      auto r = csb.ac_record();
//      snprintf(r->data(), r->size(), "...", ...);
//      // or
//      memcpy(r->data(), ???, r->size());
//    }  // the record is finalized when r goes out-of-scope
//      
//  A reading process accessing this record in the shared file will
//  see an 'invalid' record from the time ac_record() is called
//  until 'r' goes out of scope (or its release() method is called).
//  The record will be valid from then on - until the N'th subsequent
//  ac_record() call, at which time, the circular buffer wraps
//  around so the same memory is returned by the later call.
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
//  can copy records from the file with:
//    bool copyrecord(i, dest),
//  which copies record_sz (120) bytes from the (i%N)'th record to dest
//  and returns a bool indicating whether the copied record is valid.
//  The pointer, dest, must have space for record_sz (60) bytes.
//  Readers should ignore/discard invalid data but recognize that
//  invalid data is "normal" and should not be considered a program
//  error.
//
//  Notes:
//
//  There is a low-level startrecord()/finishrecord API that works
//  with char* instead of span<char>, and that does *not* autoclose.
//  The ac_record API is preferred.
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
    static const size_t _block_sz = 128;
    static const size_t cksum_sz = 8;
    static const size_t record_sz = _block_sz - cksum_sz;
    char *baseaddr;
    core123::ac::fd_t<> fd;
    size_t N;
    bool writable;
    bool readable;
    std::atomic<uint64_t> recordctr = 0;

    circular_shared_buffer(const std::string& filename, int flags, size_t _N=1024*1024/_block_sz, int mode=0600){
        fd = sew::open(filename.c_str(), flags, mode);
        writable = (flags&O_RDWR) | (flags&O_WRONLY);
        readable = !(flags&O_WRONLY); // ! whose bright idea was it to make O_RDONLY==0 ??
        struct stat sb;
        sew::fstat(fd, &sb);
        size_t filesz = sb.st_size;
        bool just_created = (flags&O_CREAT) && (filesz == 0);
        int mmflags;
        if(writable){
            bool do_truncate = (flags&O_TRUNC) || just_created;
            if(do_truncate){
                filesz = _N * _block_sz;
                if(filesz%getpagesize())
                    throw std::runtime_error("circular_shared_buffer: file size must be divisible by pagesize");
                sew::ftruncate(fd, 0);
                sew::ftruncate(fd, filesz);
            }
            mmflags = PROT_WRITE;
            if(readable)
                mmflags |= PROT_READ;
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
        if(filesz==0 || filesz%getpagesize() || filesz%_block_sz)
            throw std::runtime_error("circular_shared_buffer: file size must be non-zero and divisible by pagesize");
        baseaddr = (char *)sew::mmap(nullptr, filesz, mmflags, MAP_SHARED, fd, 0);
        N = filesz / _block_sz;
    }            
        
    ~circular_shared_buffer(){
        // Don't use sew.  We can't throw.
        ::munmap(baseaddr, N*_block_sz);
    }

    char *startrecord() {
        if(!writable)
            throw std::runtime_error("circular_shared_buffer: not writable");
        char *ret = baseaddr + (recordctr.fetch_add(1)%N)*_block_sz;
        ::memset(&ret[record_sz], '\f', cksum_sz);
        std::atomic_thread_fence(std::memory_order_release);
        return ret;
    }
    
    void finishrecord(char *record) {
        if(!writable)
            throw std::runtime_error("circular_shared_buffer: not writable");
        printable_cksum16(record, record_sz, &record[record_sz]);
        std::atomic_thread_fence(std::memory_order_release);
    }

    auto ac_record(){
        using S=tcb::span<char>;
        return core123::make_autocloser(new S(startrecord(), record_sz),
                               [this](S *p){
                                   finishrecord(p->data());
                                   delete p;
                               });
    }

    // printable_cksum writes a cksum_sz (4-byte) checksum each byte
    // of which is an ascii digit, to dest.  Think of it as a crc16,
    // written out as four hex digits.
    static void printable_cksum16(void *p, size_t len, char* dest){
        // threeroe is overkill, but there's no crc16 in libc.
        uint64_t h = threeroe(p, len).hash64();
        for(size_t i=0; i<cksum_sz; ++i){
            dest[i] = "0123456789abcdef"[h&0xf];
            h >>= 4;
        }
    }

    bool copyrecord(size_t i, char *dest) const {
        if(!readable)
            throw std::runtime_error("circular_shared_buffer: not readable");
        char *record = baseaddr + _block_sz*(i%N);
        char storedsum[cksum_sz];
        std::atomic_thread_fence(std::memory_order_acquire);
        ::memcpy(dest, record, record_sz);
        ::memcpy(storedsum, &record[record_sz], cksum_sz);
        char computedsum[cksum_sz];
        printable_cksum16(dest, record_sz, computedsum);
        bool ret = !::memcmp(storedsum, computedsum, cksum_sz);
        return ret;
    }

    size_t nrecords() const { return N; }
    bool is_readable() const { return readable; }
    bool is_writable() const { return writable; }

    // volatile_record_addr - Not recommended!  But it might be useful if
    // somebody wants to do their own polling, checksum-ing or
    // debugging.
    char *volatile_record_addr(size_t i) const{ return baseaddr + _block_sz*(i%N); }
};
    
} // namespace core123

#pragma once
#include <core123/sew.hpp>
#include <core123/threeroe.hpp>
#include <core123/svto.hpp>
#include <atomic>
#include <cstring>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <sys/mman.h>

// circular_shared_buffer - a shared-memory, file-backed, thread-safe
//   circular buffer, very loosely inspired by Varnish's log system.
//
// Quick start:
//
// In the writing process, create a new file for writing with 1024
// 128-byte records:
//
//   circular_shared_buffer writer("/tmp/mycsb", O_WRONLY|O_CREAT|O_TRUNC, 1024);
//
// In other threads in the writing process (no synchronization required):
//
//   writer.append(sv);
//                                 
// Where sv is a str_view (or something that can be converted to
// str_view).  If sv is larger than 120 (the data_size()), it will
// be truncated, and if its smaller, it will be padded with spaces.
//
// In one or more reader processes or threads:
//
//   circular_shared_buffer reader("/tmp/mycsb", O_RDONLY);
//
//   std::string rec = reader.copyrecord(i);
//   if(!rec.empty()){
//      ... rec is a copy of the (i%csb.size())'th record in the file ...
//   }
//     
// Note that copyrecord occasionally returns an empty record, meaning
// that the selected record has either never been written, or was in
// the process of being written when copyrecord was called.  This is
// not a program error.

// Details:
//
//  The constructor has two required and three optional arguments:
//   circular_shared_buffer(filename, oflags, N=0, recordsz=0, mode=0600)
//
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
//  Each record contains record_size() bytes, of which 8 (aka
//  cksum_size) are reserved for an automatically generated
//  checksum.  The size of the data payload in each record is given by
//  data_size() (equal to record_size() - 8).  The
//  record_size() is determined as follows:
//    -if the constructor argument is non-zero, then use it.
//    -else, if the file already exists and has an extended attribute
//     named user.circular_shared_buffer.record_size, then convert
//     that using svto<size_t> and use that.
//    -else, use static const circular_shared_buffer::default_record_size (128).
//  It is an error if the resulting record_size() is less than 8.
//  It is an error if the total file size (N * record_size()) is not
//  divisible by get_pagesize() (typically 4096).
//
//  When the constuctor creates a new file, it tries to add an
//  extended attribute named user.circular_shared_buffer.record_size,
//  but it is not an error if it is unable to do so.
//
//  Also note that it's possible for the constructor to throw after
//  filename is opened.  If oflags contains O_TRUNC or O_CREAT, the
//  visible state of the filesystem may be changed.  Consequently,
//  the constructor has "basic" but not "strong"  exception safety.
//
//  If the file is opened for writing (O_RDWR, O_WRONLY is silently
//  promoted to O_RDWR), the program may call the member function:
//
//    void append(str_view, char fill=' ')
//      
//  which writes the contents of str_view into the next available
//  record in the file.  If str_view.size() > data_size(), only the
//  first data_size() bytes are written.  If str_view.size() is less
//  than data_size(), the remaining bytes are padded with fill.
//
//  The append() method is thread-safe.  Any number of threads can
//  append concurrently.
//
//  A reading process accessing written records with the member
//  function:
//
//    std::string copyrecord(size_t pos)
//
//  which copies the (pos%N)'th record from the file.  Note that pos
//  is automatically reduced modulo N, so the caller need not be aware
//  of how many records are in the file.  The string returned by
//  copyrecord will either have size equal to data_size(), or it will
//  be empty, indicating that the record is currently 'invalid'.  The
//  i'th record is invalid until append() has been returned i times,
//  after which it remains valid until append() is called N more
//  times.  Then it is invalid for a short time while append() runs,
//  and it becomes valid again after the N'th append() returns, but
//  with new data.
//  
//  Readers should not try to "synchronize" with writers.  Instead,
//  they should just call copyrecord() and check that the result is
//  non-empty.  They should expect that the records in the file is
//  ephemeral, and will sooner or later be overwritten by the writer.
//  Note, though that behavior is undefined (probably a segfault) if a
//  writer re-opens a file with O_TRUNC and a new value of N while a
//  reader has it open.
//
//  Informational methods and members:
//
//   size_t size() const; // returns N, the number of records
//   size_t data_size() const; // returns the number of payload/data bytes in each record
//   size_t record_size() const; // returns the size in bytes of each record
//   static const size_t cksum_size = 8 ;
//   static const size_t default_record_size = 128 ;
//
//  Notes:
//
//  Note that the contents of the records is completely at the
//  discretion of the writer.  Writers may (or may not) choose to put
//  data in the record (e.g., sequence numbers, timestamps) that allow
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
//  If the compiler doesn't support __cpp_inline_variable (e.g.,
//  gcc6) it will not be possible to take the address of cksum_size
//  or default_record_size.

//  Caveats:
//
//  Nothing is guaranteed if readers and writers access the "same"
//  file over a networked filesystem.  NFS == "Not a Filesystem".
//
//  Checksums are 32 bits (hex-encoded into 8 bytes), so collisions
//  are unlikely but not impossible.  Expect a false positive, i.e., a
//  record that is reported valid but in fact has been corrupted by a
//  concurrent writer approximately once for every 4 billion correctly
//  reported invalid records (which is probably *much* less than once
//  every 4 billion records).

#include <core123/span.hpp>

namespace core123{

struct circular_shared_buffer{
    inline static const size_t default_record_size = 128;
    inline static const size_t cksum_size = 8;
private:
    size_t record_sz;
    size_t data_sz;
    char *baseaddr;
    core123::ac::fd_t<> fd;
    size_t N;
    bool writable;
    std::atomic<uint64_t> recordctr;

public:
    circular_shared_buffer(const circular_shared_buffer&) = delete;
    circular_shared_buffer(circular_shared_buffer&&) = delete;
    circular_shared_buffer& operator=(const circular_shared_buffer&) = delete;
    circular_shared_buffer& operator=(circular_shared_buffer&&) = delete;
    
    circular_shared_buffer(const std::string& filename, int flags, size_t _N=0, size_t _record_sz = 0, int mode=0600) :
        recordctr(0)
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
            record_sz = default_record_size;
        }else{
            char rszbuf[32];
            // Look for the record size in an xattr.  If there's no
            // such xattr, use default_record_size.  A missing xattr is
            // *not* an error.
#ifndef __APPLE__
            auto rszlen = fgetxattr(fd, "user.circular_shared_buffer.record_size", rszbuf, sizeof(rszbuf));
#else
            auto rszlen = fgetxattr(fd, "user.circular_shared_buffer.record_size", rszbuf, sizeof(rszbuf), XATTR_NOFOLLOW);
#endif
            if(rszlen >= 0 && size_t(rszlen) <= sizeof(rszbuf)){
                record_sz = svto<size_t>(str_view{rszbuf, size_t(rszlen)});
            }else{
                record_sz = default_record_size;
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
                (void)fsetxattr(fd, "user.circular_shared_buffer.record_size", rsc.c_str(), rsc.size(), 0);
#else
                (void)fsetxattr(fd, "user.circular_shared_buffer.record_size", rsc.c_str(), rsc.size(), 0, 0);
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
        if(record_sz < cksum_size)
            throw std::runtime_error("circular_shared_buffer: record_sz must be at least cksum_sz (" + str(cksum_size) + ")");
        data_sz = record_sz - cksum_size;
        baseaddr = (char *)sew::mmap(nullptr, filesz, mmflags, MAP_SHARED, fd, 0);
    }            
        
    ~circular_shared_buffer(){
        // Don't use sew.  We can't throw.
        ::munmap(baseaddr, N*record_sz);
    }

    void append(str_view sv, unsigned char fill = ' '){
        if(!writable)
            throw std::runtime_error("circular_shared_buffer: not writable");
        auto dest = baseaddr + (recordctr.fetch_add(1)%N)*record_sz;
        ::memcpy(dest, sv.data(), std::min(sv.size(), data_sz));
        if(data_sz > sv.size())
            ::memset(&dest[sv.size()], fill, data_sz-sv.size());
        printable_cksum(dest, data_sz, &dest[data_sz]);
        std::atomic_thread_fence(std::memory_order_release);
    }
    
    std::string copyrecord(size_t i) const {
        auto rec = baseaddr + record_sz*(i%N);
        std::atomic_thread_fence(std::memory_order_acquire);
        std::string ret(rec, record_sz);
        char computedsum[cksum_size];
        printable_cksum(&ret[0], data_sz, computedsum);
        if(::memcmp(&ret[data_sz], computedsum, cksum_size) != 0)
            ret.clear();        // discard everything
        else
            ret.erase(data_sz); // discard the checksum
        return ret;
    }

    size_t size() const { return N; }
    size_t record_size() const { return record_sz; }
    size_t data_size() const { return data_sz; }
    bool is_writable() const { return writable; }

    // volatile_record_addr - Not recommended!  But it might be useful if
    // somebody wants to do their own polling, checksum-ing or
    // debugging.
    char *volatile_record_addr(size_t i) const{ return baseaddr + record_sz*(i%N); }

    // printable_cksum writes a cksum_sz checksum each byte
    // of which is an ascii digit, to dest.  Think of it as a crc,
    // written out as hex digits.
    static void printable_cksum(void *p, size_t len, char* dest){
        // threeroe is overkill, but there's no crc32 in libc.
        uint64_t h = threeroe(p, len).hash64();
        for(size_t i=0; i<cksum_size; ++i){
            dest[i] = "0123456789abcdef"[h&0xf];
            h >>= 4;
        }
    }

};
    
} // namespace core123

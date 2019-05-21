"""Support for side-by-side read-only operations on paths  
relative to two separate roots.  Results are checked
for "equivalence".  The idea is that you do something like:

   drh = dualroot.hndl(EXPORT_ROOT, MTPT)

   fd = drh.open(...)
   txt = drh.read(fd, ...)
   drh.lseek(fd, ...)
   ...

Each of the drh methods works on file descriptors/paths
in both roots, and throws a MismatchException if the results
differ.  If you don't see a MismatchException, you
know you've confirmed "consistency".

TODO:
- figure out when discrepancies are permissible
  (before settling time).  This probably requires
  some 'write' capabilities, including rename, unlink,
  mkdir, etc.  An ambitious version would also consult
  the long-timeout database, but that can come later.
- figure out when ESTALE is ok?
"""

import os
from stat import *

opj = os.path.join

def _open(path, flags):
    try:
        return os.open(path, flags)
    except OSError as e:
        return e

def _read(fd, len):
    try:
        return os.read(fd, len)
    except OSError as e:
        return e

def _pread(fd, len, off):
    try:
        try:
            return os.pread(fd, len, off)
        except AttributeError:
            off0 = os.lseek(fd, 0, os.SEEK_CUR)
            os.lseek(fd, off, os.SEEK_SET)
            ret = os.read(fd, len)
            os.lseek(fd, off0, os.SEEK_SET)
            return ret
    except OSError as e:
        return e

def _close(fd):
    try:
        return os.close(fd)
    except OSError as e:
        return e

def _fstat(fd):
    try:
        return os.fstat(fd)
    except OSError as e:
        return e

def _stat(path):
    try:
        return os.stat(path)
    except OSError as e:
        return e

def _lstat(path):
    try:
        return os.lstat(path)
    except OSError as e:
        return e

def _lseek(fd, pos, how):
    try:
        return os.lseek(fd, pos, how)
    except OSError as e:
        return e

# python doesn't have os.opendir/readdir/closedir.  All
# it has is listdir.
def _listdir(path):
    try:
        return os.listdir(path)
    except OSError as e:
        return e

# In python3.3 we get getxattr and listxattr.  Is that
# enough reason to use py3??

class MismatchException(Exception):
    def __init__(self, msg, a, b):
        self.message = msg
        self.a = a
        self.b = b
        super(MismatchException, self).__init__(msg, a, b)

def chk(a, b, msg):
    if a == b:
        return a
    if type(a) == OSError and type(b) == OSError and a.errno == b.errno:
        return a
    raise MismatchException(msg, a, b)

def chkstat(a, b, msg):
    for i in (ST_NLINK, ST_UID, ST_GID, ST_SIZE, ST_MTIME, ST_CTIME):
        if a[i] != b[i]:
            raise MismatchException(msg, a, b)
    # don't check ST_MODE because of 'massage_attributes'
    return a 

class Hndl:
    def __init__(self, r1, r2):
        self.r1 = r1
        self.r2 = r2

    def open(self, rel, flags):
        r1 = _open(opj(self.r1, rel), flags)
        r2 = _open(opj(self.r2, rel), flags)
        if type(r1)==OSError or type(r2)==OSError:
            chk(r1,r2, "open")
        return (r1, r2)

    def close(self, fdpair):
        r1 = _close(fdpair[0])
        r2 = _close(fdpair[1])
        return chk(r1, r2, "close")

    def read(self, fdpair, n):
        r1 = _read(fdpair[0], n)
        r2 = _read(fdpair[1], n)
        return chk(r1, r2, "read")
    
    def readlink(self, path):
        r1 = _readlink(opj(self.r1, path))
        r2 = _readlink(opj(self.r2, path))
        return chk(r1, r2, "readlink")

    def stat(self, path):
        r1 = _stat(opj(self.r1, path))
        r2 = _stat(opj(self.r2, path))
        return chkstat(r1, r2, "stat")

    def fstat(self, fdpair):
        r1 = _fstat(fdpair[0])
        r2 = _fstat(fdpair[1])
        return chkstat(r1, r2, "fstat")

    def listdir(self, path):
        r1 = _listdir(opj(self.r1, path))
        r2 = _listdir(opj(self.r2, path))
        # WARNING - the top-level directory will contain 
        # .fs123_statistics, etc. in one but not the other...
        return chk(r1, r2, "listdir")

    def lseek(self, fdpair, pos, how):
        r1 = _lseek(fdpair[0], pos, how)
        r2 = _lseek(fdpair[1], pos, how)
        return chk(r1, r2, "lseek")

    def pread(self, fdpair, n, off):
        r1 = _pread(fdpair[0], n, off)
        r2 = _pread(fdpair[1], n, off)
        return chk(r1, r2, "pread")

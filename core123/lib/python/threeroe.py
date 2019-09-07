#!/usr/bin/python
'''Very simple wrapper around ThreeRoe using Python ctypes
   because ctypes is portable across python2, python3 and pypy
   whereas a C Extension is not.

   ThreeRoe class has the standard hashlib interface, should
   work fine for Python2.7 and Python3.
'''

from __future__ import print_function
import os, ctypes

__version__ = '0.1'
__author__ = 'Mark Moraes, D. E. Shaw Research'

libdir_ = os.path.dirname(__file__)
#if libdir_ in (os.path.abspath('.'), '.', ''): libdir_ = 'obj'
# Assume 
m_ = ctypes.cdll.LoadLibrary(os.path.join(libdir_, 'py_threeroe.so'))
_tr_create = m_.tr_create
_tr_create.restype = ctypes.c_void_p
_tr_create.argtypes = ctypes.POINTER(ctypes.c_char), ctypes.c_size_t

_tr_copy = m_.tr_copy
_tr_copy.restype = ctypes.c_void_p
_tr_copy.argtypes = ctypes.c_void_p,

_tr_update = m_.tr_update
_tr_update.restype = ctypes.c_void_p
_tr_update.argtypes = ctypes.c_void_p, ctypes.POINTER(ctypes.c_char), ctypes.c_size_t

_tr_free = m_.tr_free
_tr_free.restype = None
_tr_free.argtypes = ctypes.c_void_p,

_tr_digest = m_.tr_digest
_tr_digest.restype = None
# N.B.  the C++ _tr_digest would prefer ctypes.c_ubyte, but ctypes.create_string_buffer
# gives us a POINTER(ctypes.c_char), and it's easier to cast in C++.
_tr_digest.argtypes = ctypes.c_void_p, ctypes.POINTER(ctypes.c_char)

_tr_hexdigest = m_.tr_hexdigest
_tr_hexdigest.restype = None
_tr_hexdigest.argtypes = ctypes.c_void_p, ctypes.POINTER(ctypes.c_char)

_bempty = bytes()
_btype = type(_bempty)

class ThreeRoe:
    block_size = 16
    digest_size = 16
    name = 'ThreeRoe'
    # NOTE: have to save a ref to _tr_free in this class because in Python2.7,
    # the module-level _tr_free gets set to None before this object __del__
    # method is called, resulting in a warning
    # Exception TypeError: "'NoneType' object is not callable" in <bound method ThreeRoe.__del__...>
    # and the deleter is not called. Annoying (though harmless...)
    _deleter = _tr_free
    def __init__(self, s = None):
        if s == None: s = _bempty
        buf = ctypes.c_char_p(s)
        self._tr = _tr_create(buf, len(s))
    def __del__(self):
        self._deleter(self._tr)
    def __enter__(self):
        return self
    def __exit__(self, t, v, tb):
        self._deleter(self._tr)
        return t is None
    def update(self, s):
        buf = ctypes.c_char_p(s)
        _tr_update(self._tr, buf, len(s))
    def hexdigest(self):
        thirtythree = 2*self.digest_size+1
        r = ctypes.create_string_buffer(thirtythree)
        _tr_hexdigest(self._tr, r)
        return r.value.decode('ascii')
    def digest(self):
        r = ctypes.create_string_buffer(self.digest_size)
        _tr_digest(self._tr, r)
        return r.value
    def copy(self):
        t = ThreeRoe()
        t._tr = _tr_copy(self._tr) 
        return t

if __name__ == '__main__':
    import sys
    rsize = 1024*1024
    for arg in sys.argv[1:]:
        with open(arg, 'rb') as f:
            with ThreeRoe() as t:
                s = f.read(rsize)
                if s == '':
                    print(t.hexdigest(), arg)
                    continue
                t.update(s)

    

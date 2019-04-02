#!/usr/bin/python2
from __future__ import print_function
from threeroe import ThreeRoe
import os, hashlib, time

if __name__ == '__main__':
    hname = os.getenv('UT_HASHNAME', None)
    if hname == None:
        Hash = ThreeRoe
    else:
        Hash = getattr(hashlib, hname)
    b1 = b'hello world\n'
    b2 = 'goodbye\n'.encode('ascii')
    t = Hash(b1)
    print('t', t.hexdigest())
    if hname == None:
        assert t.digest() == b'\x29\x1a\x9c\xc7\xcf\xa0\x01\x18\x1f\x52\x31\xc2\x6a\x7a\x62\x75'
        assert t.hexdigest() == '291a9cc7cfa001181f5231c26a7a6275'
    u = Hash()
    print('initial u', u.hexdigest())
    if hname == None:
        assert u.digest() == b'\x09\x99\x8c\xa1\x97\xa7\xc2\x33\x24\x99\x53\xe3\x66\xb2\x7e\x88'
        assert u.hexdigest() == '09998ca197a7c233249953e366b27e88'
    u.update(b1)
    print('u', u.hexdigest())
    if hname == None:
        assert u.digest() == b'\x29\x1a\x9c\xc7\xcf\xa0\x01\x18\x1f\x52\x31\xc2\x6a\x7a\x62\x75'
        assert u.hexdigest() == '291a9cc7cfa001181f5231c26a7a6275'
    else:
        assert u.digest() == t.digest() and u.hexdigest() == t.hexdigest()
    v = t.copy()
    if hname == None:
        assert v.digest() == b'\x29\x1a\x9c\xc7\xcf\xa0\x01\x18\x1f\x52\x31\xc2\x6a\x7a\x62\x75'
        assert v.hexdigest() == '291a9cc7cfa001181f5231c26a7a6275'
    else:
        assert u.digest() == v.digest() and u.hexdigest() == v.hexdigest()
    v.update(b2)
    print('v', v.hexdigest())
    if hname == None:
        assert t.digest() == b'\x29\x1a\x9c\xc7\xcf\xa0\x01\x18\x1f\x52\x31\xc2\x6a\x7a\x62\x75'
        assert t.hexdigest() == '291a9cc7cfa001181f5231c26a7a6275'
        assert v.digest() == b'\x69\xcc\xa6\x96\x9d\xf8\x84\x57\xee\x7f\x12\x0e\xda\x8e\xea\xb7'
        assert v.hexdigest() == '69cca6969df88457ee7f120eda8eeab7'
    else:
        assert u.digest() == t.digest() and u.hexdigest() == t.hexdigest()
        assert u.digest() != v.digest() and u.hexdigest() != v.hexdigest()
    n=1
    for i in range(10):
        s=b'x'*n
        ts = time.time()
        nb = k = dt = 0
        while dt < 1.:
            x = Hash(s)
            y = Hash(s)
            z = Hash(s)
            w = Hash(s)
            dt = time.time() - ts
            k += 4
            nb += 4*n
        print("%.2f" % (nb/1e6/dt), "MB/s,", "%.2f" % (k/dt), "hashes/sec for", n, "byte input string")
        n *= 10

"""
Make sure that it's safe to enable pyston_lite after we've already been executing.
In particular we might have created and initialized some opcaches already.
"""

class C:
    pass

def f():
    c = C()
    c.x = 1
    c.x
    c.x
    c.x
    c.x
    c.x
    c.x
    c.x
    c.x
    c.x
    c.x
    C()
    C()
    C()
    C()
    C()
    C()
    C()
    C()
    C()
    C()

for i in range(10000):
    f()

import pyston_lite
pyston_lite.enable()

for i in range(10000):
    f()


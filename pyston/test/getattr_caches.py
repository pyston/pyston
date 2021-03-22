print("start")
N = 10000
def desc_tests():
    class CustomDescr(object):
        def __get__(self, obj, obj_type):
            return 23
        def __set__(self, obj, val):
            pass

    class C(object):
        custom_descr = CustomDescr()

        @property
        def prop(self):
            return 42

    def f1(x):
        for i in range(100):
            r = x.start
        return r

    def f2(x):
        for i in range(100):
            r = x.prop
        return r

    def f3(x):
        for i in range(100):
            r = x.custom_descr
        return r

    for i in range(N):
        r1 = f1(range(2, 300))
        r2 = f2(C())
        r3 = f3(C())
        assert r1 == 2
        assert r2 == 42
        assert r3 == 23
desc_tests()


def module_tests():
    import re
    def f1():
        for i in range(100):
            re.DEBUG
            re.compile(r"\d")
    for i in range(N):
        f1()
module_tests()

def getattr_tests():
    class C(object):
        class_attr = "class_attr"
        def __init__(self):
            self.instance_attr = "instance_attr"

        def __getattr__(self, attr):
            assert 0
    def f1(x):
        for i in range(100):
            x.class_attr
            x.instance_attr
            pass
    for i in range(N):
        f1(C())
getattr_tests()

import sys
C = 0
M = sys.modules[__name__]
def getattr_changing_module_test():
    def f():
        global C
        for i in range(100):
            C = C+1 # modify module dict
            tmp = M.getattr_changing_module_test
            M.getattr_changing_module_test = 42
            assert M.getattr_changing_module_test == 42
            M.getattr_changing_module_test = tmp

    for i in range(N):
        f()
getattr_changing_module_test()

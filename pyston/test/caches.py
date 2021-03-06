def new_f():
    return 2

def new_f_meth(self):
    return 2

def makeFunction():
    globals_ = {}
    exec("""
def f(o):
    return o.f()
    """, globals_)
    return globals_['f']

def makeClass():
    globals_ = {}
    exec("""
class C(object):
    def f(self):
        return 1
    """, globals_)
    return globals_["C"]


def testLoadMethodCache(iters):
    """
    Test 1: go from NULL dict to non-null dict
    """
    C = makeClass()
    f = makeFunction()
    c = C()
    for i in range(iters):
        r = f(c)
        assert r == 1
    c.f = new_f
    r = f(c)
    assert r == 2

    """
    Test 2: go from non-null split dict to null dict
    """
    C = makeClass()
    f = makeFunction()
    c = C()
    c.x = 1
    for i in range(iters):
        r = f(c)
        assert r == 1
    c2 = C()
    r = f(c2)

    """
    Test 3: go from non-null non-split dict to null dict
    """
    C = makeClass()
    f = makeFunction()
    c = C()
    c.x = 1
    c2 = C()
    c2.y = 2
    for i in range(iters):
        r = f(c2)
        assert r == 1
    c3 = C()
    r = f(c3)

    """
    Test 4: change splitdict keys
    """
    C = makeClass()
    f = makeFunction()
    c = C()
    c.x = 1
    c.f = 2
    c2 = C()
    c2.x = 1
    for i in range(iters):
        r = f(c2)
        assert r == 1
    c2.f = new_f
    r = f(c2)
    assert r == 2

    """
    Test 5: change non-splitdict version
    """
    C = makeClass()
    f = makeFunction()
    c = C()
    c.x = 1
    c.f = 2
    c2 = C()
    c2.y = 1
    for i in range(iters):
        r = f(c2)
        assert r == 1
    c2.f = new_f
    r = f(c2)
    assert r == 2

    """
    Test 6: change function
    """
    C = makeClass()
    f = makeFunction()
    c = C()
    for i in range(iters):
        r = f(c)
        assert r == 1
    C.f = new_f_meth
    # invalid version tag this time:
    r = f(c)
    assert r == 2
    # different version tag this time:
    r = f(c)
    assert r == 2

def main():
    # Scanning different number of iters to try to test different opcache/jit combinations
    for i in range(15):
        iters = int(1.5 * (1 << i))
        print("Testing with %d iters" % iters)
        testLoadMethodCache(iters)

main()

# test our load attribute cache data descriptor guarding
def test_data_descr():
    class CustomDescr(object):
        def __get__(self, obj, obj_type):
            return "CustomDescr.__get__"
        def __set__(self, obj, val):
            pass
    class C(object):
        custom_descr = CustomDescr()
    def f(x):
        return x.custom_descr
    N = 5000
    c = C()
    for i in range(N):
        assert(f(c) == "CustomDescr.__get__")
    c.__dict__["custom_descr"] = "c.custom_descr" # setting attribute on instance, should not change things
    assert(f(c) == "CustomDescr.__get__")
    del CustomDescr.__set__ # not a data descriptor any more must lookup attribute in instance
    assert(f(c) == "c.custom_descr")
test_data_descr()

# test our load attribute cache guarding for __getattr__ and __getattribute__
def test_getattr():
    class C(object):
        def __init__(self):
            self.attr = 1

        def __getattr__(self, name):
            assert 0, "should never get here"

        def getattribute(self, name):
            return "__getattribute__"
    def f(x):
        return x.attr
    c = C()
    for i in range(10000):
        assert f(c) == 1
    C.__getattribute__ = C.getattribute
    assert f(c) == "__getattribute__"
test_getattr()

# test our load attribute cache guarding for modules
def test_module():
    import re
    def f():
        return re.my_global_var
    re.my_global_var = 42
    for i in range(5000):
        assert f() == 42
    re.my_global_var = 23
    assert f() == 23
test_module()


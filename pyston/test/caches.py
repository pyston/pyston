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
        # print("Testing with %d iters" % iters)
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

# test our store attribute cache guarding
def test_set_slot():
    class C1(object):
        __slots__ = ("attr",)
        def get(self):
            return self.attr
    class C2(object):
        __slots__ = ("a", "attr",)
        def get(self):
            return -self.attr
    def f(x):
        x.attr = 42
    c = C1()
    for i in range(5000):
        f(c)
        assert c.get() == 42
    c = C2()
    f(c)
    assert c.get() == -42
test_set_slot()


def test_splitdict_unset():
    class C:
        pass

    def f(b):
        c = C()
        c.y = 1
        if not b:
            c.x = 1

        c.x

    for i in range(200):
        f(0)
    try:
        f(1)
        raise Exception()
    except AttributeError:
        pass
test_splitdict_unset()


def test_splitdict_fromtype():
    """
    tests some edge cases around getting attributes
    from the type when the instances have split dicts
    """

    class C:
        def __init__(self):
            self.x = 2
            self.y = 2

        def f(self):
            return 1

    def f():
        c2 = C()

        def g(c):
            return c.f()

        for i in range(1000):
            g(c2)

        assert g(c2) == 1
        c3 = C()
        c3.f = lambda: 2
        assert g(c3) == 2, g(c3)

        def h(c):
            return c.f()

        for i in range(1000):
            h(c3)

        assert h(c3) == 2
        assert h(c2) == 1

    f()
test_splitdict_fromtype()

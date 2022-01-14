def f():
    """A particularly challenging case for the CALL_METHOD ics:
    The LOAD_METHOD does not hit its IC, but it still puts
    a list at the place that CALL_METHOD expects a list."""
    l = []

    for i in range(10000000):
        # These three different methods use different variants of the method ic:
        try:
            l.clear()
        except TypeError:
            pass
        try:
            l.append(0)
        except TypeError:
            pass
        try:
            l.insert(0, 0)
        except TypeError:
            pass

        if i == 100000:
            class C:
                clear = []
                append = []
                insert = []
            l = C()
        elif i > 100000:
            break

f()

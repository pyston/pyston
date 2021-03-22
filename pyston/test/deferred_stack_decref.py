"""
we used to not decref deferred stack variables in case of an exception
"""
C = 0
class Foo():
    def __init__(self):
        pass

    def __del__(self):
        global C
        C += 1

def foo():
    f = Foo() + 1/0
def foo2():
    f = Foo()
    a + (f + f)
    a = 1

N = 10000
for i in range(N):
    try:
        foo()
    except:
        pass
    try:
        foo2()
    except:
        pass
assert C == 2*N
print("test finished successfully")

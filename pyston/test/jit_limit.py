# JIT compiles a new function
def test_jit_func():
    g = {}
    exec("def f(x): x", g)
    for i in range(1000):
        g["f"](i)

for i in range(1000):
    test_jit_func()


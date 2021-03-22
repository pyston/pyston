# OSR JIT compiles a new function 
def test_jit_osr_func():
    g = {}
    exec("""def f(x):
                for i in range(10000):
                    x""", g)
    g["f"](i)

for i in range(1000):
    test_jit_osr_func()


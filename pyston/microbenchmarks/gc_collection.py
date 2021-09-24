import gc
import time

def f():
    # flaskblogging_serve.py has 12702 functions alive when serving requests
    funcs = []
    for i in range(12702):
        def g():
            pass
        funcs.append(g)

    start = time.time()
    for i in range(200):
        gc.collect()
        gc.collect()
        gc.collect()
        gc.collect()
        gc.collect()
        gc.collect()
        gc.collect()
        gc.collect()
        gc.collect()
        gc.collect()
    print(time.time() - start)

f()


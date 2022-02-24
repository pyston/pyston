import sys, os

totest = [list, sys]
for obj in totest:
    for k, v in obj.__dict__.items():
        refcnt = sys.getrefcount(v)
        if refcnt > 1e9:
            continue
        print(k, type(v), v, refcnt)

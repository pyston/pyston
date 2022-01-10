I = 10000000
f = isinstance
for i in range(I):
    x = f(1, int)
    if i == I/2:
        f = lambda x, y: 42

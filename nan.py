from dis import dis
NUM_ITER = 100000
def nan_f(a, b):
    for i in range(NUM_ITER):
        #(a + b) + 2 + 3 + 4 + 5 + 6 + 7
        (a + b) + 2. + 3. + 4. + 5. + 6. + 7.
f = nan_f
dis(f)
def g():
    for i in range(1000):
        #f(1000, 2000)
        #f(1, 2)
        f(1000., 2000.)
g()

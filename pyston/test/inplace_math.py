# check that the inplace float modification optimization
# generates same results
def add():
    a = 1.5
    b = -7.4
    c = 4.2
    r = a + b
    r = r + c
    assert a + b + c == r
def sub():
    a = 1.5
    b = -7.4
    c = 4.2
    r = a - b
    r = r - c
    assert a - b - c == r
def mul():
    a = 1.5
    b = -7.4
    c = 4.2
    r = a * b
    r = r * c
    assert a * b * c == r
for i in range(10000):
    add()
    sub()
    mul()

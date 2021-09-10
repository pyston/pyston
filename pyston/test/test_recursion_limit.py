import sys
import threading
import time

def measure():
    "Measures the current stack limit of python frames"
    x = [0]

    def inner():
        x[0] += 1
        inner()

    try:
        inner()
    except RecursionError:
        pass

    return x[0]

if __name__ == "__main__":
    set_limit = 1024

    # Basic test: check that setting the limit is close
    sys.setrecursionlimit(set_limit)
    actual_limit = measure()
    assert set_limit - 3 <= actual_limit < 2 * set_limit, actual_limit

    # Test that doubling the limit roughly doubles the number of frames
    sys.setrecursionlimit(set_limit * 2)
    actual_limit2 = measure()
    assert 1.9 * actual_limit - 1 <= actual_limit2 <= 2.1 * actual_limit + 2, (actual_limit, actual_limit2)

    # Threading test:
    # Check that 1) recursion limits are inherited by new threads, and 2) changing the
    # limit on one thread changes it for other threads.
    sys.setrecursionlimit(100)
    def test():
        assert measure() < 200
        sys.setrecursionlimit(1024)
    t = threading.Thread(target=test)
    t.start()
    t.join()
    assert measure() > 1000

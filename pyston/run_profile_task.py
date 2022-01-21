import fasteners
import os
import subprocess
import sys

exe = os.path.abspath(sys.executable)

def run(bench, *args, retries=3):
    for i in range(retries):
        code = subprocess.call([exe, bench] + list(args))
        if code == 0:
            return
    raise Exception((bench, args, code))

if __name__ == "__main__":
    os.chdir(os.path.dirname(__file__))

    lock = fasteners.InterProcessLock("/tmp/pyston_pgo.lock")
    with lock:
        run("macrobenchmarks/benchmarks/bm_djangocms/run_benchmark.py", "--legacy") # approx 21s
        run("macrobenchmarks/benchmarks/bm_flaskblogging/run_benchmark.py", "--legacy") # approx 10s

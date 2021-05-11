import os
import subprocess
import sys

exe = os.path.abspath(sys.executable)

def run(bench, *args):
    subprocess.check_call([exe, bench] + list(args))

if __name__ == "__main__":
    os.chdir(os.path.dirname(__file__))

    run("macrobenchmarks/benchmarks/djangocms.py", "800") # approx 21s
    run("macrobenchmarks/benchmarks/flaskblogging.py", "1800") # approx 10s

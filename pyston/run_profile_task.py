import os
import subprocess
import sys

exe = sys.executable

def run(bench, *args):
    subprocess.check_call(["/usr/bin/time", exe, bench] + list(args))

if __name__ == "__main__":
    os.chdir(os.path.dirname(__file__))

    run("macrobenchmarks/benchmarks/djangocms.py", "800") # approx 21s
    run("macrobenchmarks/benchmarks/flaskblogging.py", "1800") # approx 10s

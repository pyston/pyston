import os
import subprocess
import sys

PYPERF = os.path.join(os.path.dirname(__file__), "../build/system_env/bin/pyperf")
def run(args, output_name, n=3):
    os.makedirs("results", exist_ok=True)
    subprocess.check_call([PYPERF, "command", "-w", "0", "-l", "1", "-p", "1", "-n", str(n), "-v", "-o", "results/%s.json" % output_name] + args)

def show(output_name):
    subprocess.check_call([PYPERF, "stats", "results/%s.json" % output_name])

if __name__ == "__main__":
    for env in ["system", "pypy", "stock", "bc", "unopt", "opt"]:
        print(env)
        run(["build/%s_env/bin/python" % env, sys.argv[1]], env, n=1)
        print()

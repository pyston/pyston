import os
import shutil
import subprocess
import sys
import sysconfig
import tempfile

def rel(path):
    return os.path.join(os.path.dirname(__file__), path)

if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as tempdir:
        subprocess.check_call([sys.executable, "-m", "venv", os.path.join(tempdir, "env")])
        # I'm not sure if --nobinary :all: is exactly what we want, we get a lot of messages like
        # Skipping wheel build for pycparser, due to binaries being disabled for it.
        subprocess.check_call([os.path.join(tempdir, "env/bin/pip"), "install", "--no-binary", ":all:", "-r", rel("../benchmark_requirements.txt"), "-r", rel("../benchmark_requirements_nonpypy.txt")])

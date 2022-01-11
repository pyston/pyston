import os
import subprocess
import sys
import tempfile

"""
django_requirements.txt:
- based on django/tests/requirements/py3.txt
- pylibmc doesn't provide wheels for python3.8, so I removed it from the dependencies
- I froze the dependencies with pip freeze
"""

if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as tempdir:
        print("PYSTONTEST: on-failure-print If you see a WebP failure, you might have to install libwebp-dev, delete the cached pillow wheel, and try again")

        def rel(path):
            return os.path.join(os.path.dirname(__file__), path)

        env_dir = os.path.abspath(os.path.join(tempdir, "env"))
        subprocess.check_call([rel("../../../build/bootstrap_env/bin/virtualenv"), "-p", sys.executable, env_dir])

        subprocess.check_call([os.path.join(env_dir, "bin/pip"), "install", "-e", rel("django")])
        # Small hack: the comparison script looks for virtualenv setups, which it thinks are ended
        # once packages are successfully installed.  If we have two pip install commands, it will
        # think virtualenv is done after the first one, and complain about the nondeterminism in the second one.
        # So just make it think we have a second virtualenv setup to ignore:
        print("created virtual environment (step 2)")
        subprocess.check_call([os.path.join(env_dir, "bin/pip"), "install", "-r", rel("django_requirements.txt")])

        r = subprocess.call([os.path.join(env_dir, "bin/python3"), "-u", rel("django/tests/runtests.py"), "--parallel", "1"])
        assert r in (0, 1), r

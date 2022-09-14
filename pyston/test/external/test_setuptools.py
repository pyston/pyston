import helpers
import os
import shutil
import subprocess
import sys
import tempfile

"""
setuptools_requirements.txt is frozen from  setuptools/tests/requirements.txt
"""

if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as tempdir:
        def rel(path):
            return os.path.join(os.path.dirname(__file__), path)

        env_dir = os.path.abspath(os.path.join(tempdir, "env"))
        subprocess.check_call([rel("../../../build/bootstrap_env/bin/virtualenv"), "-p", sys.executable, env_dir])

        # Setuptools setup writes files into its source directory, so create a clean copy of it
        setuptools_dir = os.path.join(tempdir, "setuptools")
        shutil.copytree(rel("setuptools"), setuptools_dir)

        subprocess.check_call([os.path.join(env_dir, "bin/pip"), "install", "-r", rel("setuptools_requirements.txt")])

        if helpers.has_pyston_lite():
            helpers.install_pyston_lite_into(os.path.join(env_dir, "bin/python3"))

        # Small hack: the comparison script looks for virtualenv setups, which it thinks are ended
        # once packages are successfully installed.  If we have two pip install commands, it will
        # think virtualenv is done after the first one, and complain about the nondeterminism in the second one.
        # So just make it think we have a second virtualenv setup to ignore:
        print("created virtual environment (step 2)")
        subprocess.check_call([os.path.join(env_dir, "bin/python"), "bootstrap.py"], cwd=setuptools_dir)
        subprocess.check_call([os.path.join(env_dir, "bin/pip"), "install", "-e", setuptools_dir])

        r = subprocess.call([os.path.join(env_dir, "bin/pytest")], cwd=setuptools_dir)
        assert r in (0, 1), r

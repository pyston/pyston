import helpers
import os
import shutil
import subprocess
import sys
import sysconfig
import tempfile

# test requires Python >= 3.8
if __name__ == "__main__" and sys.version_info[:2] >= (3, 8):
    with tempfile.TemporaryDirectory() as tempdir:
        # Pandas has some tests that look flaky and are marked xfail
        print("PYSTONTEST: no-log-check")
        print("PYSTONTEST: allow-difference xfailed 5")
        print("PYSTONTEST: allow-difference passed 5")

        def rel(path):
            return os.path.join(os.path.dirname(__file__), path)

        env_dir = os.path.abspath(os.path.join(tempdir, "env"))
        subprocess.check_call([rel("../../../build/bootstrap_env/bin/virtualenv"), "-p", sys.executable, env_dir])

        pandas_dir = os.path.join(tempdir, "pandas")
        shutil.copytree(rel("pandas"), pandas_dir)

        # pandas' test_hashtable.py uses tracemalloc, so just place an empty file
        # that will make sure that CPython and Pyston fail in the same way:
        open(os.path.join(pandas_dir, "tracemalloc.py"), 'w').write('\n')

        # pandas provides requirements-dev.txt, but I was unable to pip-install this file
        # So instead, I created a fresh env and did
        # pip install pandas boto3 botocore hypothesis moto flask pytest pytest-cov pytest-xdist pytest-asyncio pytest-instafail
        # pip uninstall pandas
        # pip freeze
        subprocess.check_call([os.path.join(env_dir, "bin/pip"), "install", "-r", rel("pandas_requirements.txt")])

        if helpers.has_pyston_lite():
            helpers.install_pyston_lite_into(os.path.join(env_dir, "bin/python3"))

        subprocess.check_call([os.path.join(env_dir, "bin/python"), "setup.py", "develop"], cwd=pandas_dir)

        r = subprocess.call([os.path.join(env_dir, "bin/pytest"), "pandas", "--skip-slow", "--skip-network", "--skip-db", "-m", "not single", "-n", "0", "-r", "sxX"], cwd=pandas_dir)
        assert r in (0, 1), r

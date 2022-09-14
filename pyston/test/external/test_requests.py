import helpers
import os
import subprocess
import sys
import tempfile

"""
requests_requirements.txt comes from requests/setup.py: requires+test_requirements, as well as pygments
"""

if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as tempdir:
        print("PYSTONTEST: no-log-check")

        def rel(path):
            return os.path.join(os.path.dirname(__file__), path)

        env_dir = os.path.abspath(os.path.join(tempdir, "env"))
        subprocess.check_call([rel("../../../build/bootstrap_env/bin/virtualenv"), "-p", sys.executable, env_dir])

        subprocess.check_call([os.path.join(env_dir, "bin/pip"), "install", "-r", rel("requests_requirements.txt")])

        if helpers.has_pyston_lite():
            helpers.install_pyston_lite_into(os.path.join(env_dir, "bin/python3"))

        # requests has some nondeterministic output
        r = subprocess.call([os.path.join(env_dir, "bin/python"), "-u", "setup.py", "test"], cwd=rel("requests"))
        assert r in (0, 1)

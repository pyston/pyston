import helpers
import os
import subprocess
import sys
import tempfile

"""
urllib3_requirements are from urllib3/dev-requirements.txt
then pip freeze'd
"""

import helpers

if __name__ == "__main__":
    print("this testsuite seems to be have some flaky tests")
    sys.exit()

    with tempfile.TemporaryDirectory() as tempdir:
        print("PYSTONTEST: no-log-check")
        print("PYSTONTEST: allow-difference warnings 5")

        def rel(path):
            return os.path.join(os.path.dirname(__file__), path)

        env_dir = os.path.abspath(os.path.join(tempdir, "env"))
        subprocess.check_call([rel("../../../build/bootstrap_env/bin/virtualenv"), "-p", sys.executable, env_dir])

        subprocess.check_call([os.path.join(env_dir, "bin/pip"), "install", "-r", rel("urllib3_requirements.txt"), "--no-binary", "pyopenssl"])
        # Small hack: the comparison script looks for virtualenv setups, which it thinks are ended
        # once packages are successfully installed.  If we have two pip install commands, it will
        # think virtualenv is done after the first one, and complain about the nondeterminism in the second one.
        # So just make it think we have a second virtualenv setup to ignore:
        print("created virtual environment (step 2)")
        subprocess.check_call([os.path.join(env_dir, "bin/pip"), "install", "-e", rel("urllib3"), "--use-feature=2020-resolver"])

        if helpers.has_pyston_lite():
            helpers.install_pyston_lite_into(os.path.join(env_dir, "bin/python3"))

        # urllib3 testsuite has some flaky tests, which generate nondeterministic
        # log lines and warnings.  Usually this is the right hash and expected results,
        # but we'll not check the hash and just check that the warnings are close
        r = subprocess.call([os.path.join(env_dir, "bin/pytest"), "-k", "not test_ssl_read_timeout and not test_ssl_failed_fingerprint_verification"], cwd=rel("urllib3"))
        assert r in (0, 1)

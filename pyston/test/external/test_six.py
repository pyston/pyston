import helpers
import os
import subprocess
import sys
import tempfile

if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as tempdir:
        def rel(path):
            return os.path.join(os.path.dirname(__file__), path)

        env_dir = os.path.abspath(os.path.join(tempdir, "env"))
        subprocess.check_call([rel("../../../build/bootstrap_env/bin/virtualenv"), "-p", sys.executable, env_dir])

        subprocess.check_call([os.path.join(env_dir, "bin/pip"), "install", "pytest"])

        if helpers.has_pyston_lite():
            helpers.install_pyston_lite_into(os.path.join(env_dir, "bin/python3"))

        r = subprocess.call([os.path.join(env_dir, "bin/pytest"), "-rfsxX"], cwd=rel("six"))
        assert r in (0, 1), r

import helpers
import os
import shutil
import subprocess
import sys
import tempfile

if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as tempdir:
        # The log seems somewhat non-deterministic:
        print("PYSTONTEST: no-log-check")

        def rel(path):
            return os.path.join(os.path.dirname(__file__), path)

        # sqlalchemy currently has a bug where the test suite will fail
        # if a parent directory is named "test". So copy it into the temp dir
        # https://github.com/sqlalchemy/sqlalchemy/issues/7045
        sqlalchemy_dir = os.path.join(tempdir, "sqlalchemy")
        shutil.copytree(rel("sqlalchemy"), sqlalchemy_dir)

        env_dir = os.path.abspath(os.path.join(tempdir, "env"))
        subprocess.check_call([rel("../../../build/bootstrap_env/bin/virtualenv"), "-p", sys.executable, env_dir])

        subprocess.check_call([os.path.join(env_dir, "bin/pip"), "install", "pytest", "tox"])

        if helpers.has_pyston_lite():
            helpers.install_pyston_lite_into(os.path.join(env_dir, "bin/python3"))

        # r = subprocess.call([os.path.join(env_dir, "bin/pytest"), "--db", "sqlite"], cwd=rel("sqlalchemy"))
        env = dict(os.environ)
        env["TOX_WORKERS"] = "-n0"
        r = subprocess.call([os.path.join(env_dir, "bin/tox"), "--parallel", "0", "-e", "py38-sqlite"], cwd=sqlalchemy_dir, env=env)
        assert r in (0, 1), r

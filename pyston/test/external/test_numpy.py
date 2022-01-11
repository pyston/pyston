import os
import shutil
import subprocess
import sys
import sysconfig
import tempfile


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as tempdir:
        # Numpy has a flaky test: numpy/tests/test_scripts:test_f2py
        # If you have numpy installed already, this test will call the installed version
        # but have it try to load the freshly-built numpy libraries.
        # Since the installed version will invoke the system python, this test
        # will fail in pyston since it won't be able to load the pyston extension module
        # Luckily they marked it as xfail so we can just allow some of those
        print("PYSTONTEST: no-log-check")
        print("PYSTONTEST: allow-difference xpassed 5")
        print("PYSTONTEST: allow-difference xfailed 5")

        def rel(path):
            return os.path.join(os.path.dirname(__file__), path)

        env_dir = os.path.abspath(os.path.join(tempdir, "env"))
        subprocess.check_call([rel("../../../build/bootstrap_env/bin/virtualenv"), "-p", sys.executable, env_dir])

        # Numpy builds into its source directory, so create a clean copy of it
        numpy_dir = os.path.join(tempdir, "numpy")
        shutil.copytree(rel("numpy"), numpy_dir)

        # numpy's setup.py calls git commands, so write out the .git
        # file so that git knows where to look
        with open(os.path.join(numpy_dir, ".git"), "w") as f:
            f.write("gitdir: %s" % os.path.abspath(rel("../../../.git/modules/pyston/test/external/numpy")))

        subprocess.check_call([os.path.join(env_dir, "bin/pip"), "install", "-r", rel("numpy/test_requirements.txt")])

        libdir = "python" + sysconfig.get_config_var("VERSION")

        # Numpy now has pyston-detection code in it, which turns off some tests.
        # One option could be to allow differences in the test counts, but for
        # now let's try forcing the numpy testsuite to think it's running on Pyston
        # even for the baseline run.
        with open(os.path.join(env_dir, "lib/%s/site-packages/pretend_to_be_pyston.pth" % libdir), 'w') as f:
            f.write("import sys; sys.pyston_version_info = getattr(sys, 'pyston_version_info', ())")

        r = subprocess.call([os.path.join(env_dir, "bin/python"), "-u", "runtests.py", "--mode=full", "-v"], cwd=numpy_dir)
        assert r in (0, 1)

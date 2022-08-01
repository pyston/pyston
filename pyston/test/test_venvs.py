from os.path import join
import subprocess
import sys
import tempfile

def getOutput(args, **kw):
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kw)
    out, _ = p.communicate()
    assert p.wait() == 0, out.decode('utf8')
    return out

def testEnv(dir):
    exe = join(dir, "bin/python3")
    pip = join(dir, "bin/pip")
    subprocess.check_call([pip, "--version"])

    # Pillow fails to install with old versions of setuptools
    subprocess.check_call([pip, "install", "Pillow==8.0.1"])
    subprocess.check_call([exe, "-c", "import PIL; print(PIL.__version__)"])

    # gevent used to error on build, though it wasn't an environment issue
    subprocess.check_call([pip, "install", "gevent==21.12.0"])
    subprocess.check_call([exe, "-c", "import gevent; print(gevent.__version__)"])

    # PYSTON_UNSAFE_ABI used to not work with 'pyston -m venv' since that would
    # install an old version of pip which detected abi tags a different way
    subprocess.check_call([pip, "install", "numpy==1.19.4"])
    subprocess.check_call([exe, "-c", "import numpy; print(numpy.__version__)"])

if __name__ == "__main__":
    exe = sys.executable

    def testVirtualenv(version):
        with tempfile.TemporaryDirectory() as td:
            print("Testing virtualenv %s" % version)

            bootstrap_env = join(td, "bootstrap_env")
            subprocess.check_call(["virtualenv", "-p", "python3", bootstrap_env])
            subprocess.check_call([join(bootstrap_env, "bin/pip"), "install", "virtualenv==%s" % version])

            env_dir = join(td, "env")
            subprocess.check_call([join(bootstrap_env, "bin/virtualenv"), "-p", exe, env_dir])

            testEnv(env_dir)
        print()

    # testVirtualenv("15.1.0") # ubuntu virtualenv
    testVirtualenv("20.0.0") # the first virtualenv we support

    with tempfile.TemporaryDirectory() as td:
        print("Testing venv module")
        env_dir = join(td, "env")
        subprocess.check_call([exe, "-m", "venv", env_dir])

        testEnv(env_dir)
        print()

import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

def check_call(args, **kw):
    print("check_call", " ".join([repr(a) for a in args]), kw)
    return subprocess.check_call(args, **kw)

def check_output(args, **kw):
    print("check_output", " ".join([repr(a) for a in args]), kw)
    return subprocess.check_output(args, **kw)

def bolt_wheel(wheel):
    with tempfile.TemporaryDirectory() as dir:
        envdir = os.path.join(dir, "bolt_env")
        unzipdir = os.path.join(dir, "unzip")

        check_call(["unzip", wheel, "-d", unzipdir])
        os.rename(wheel, wheel + ".prebolt")

        sos = glob.glob(os.path.join(unzipdir, "*.so"))
        assert len(sos) == 1, sos
        so, = sos
        os.rename(so, so + ".prebolt")

        python_version = re.search("-cp(\d{2,3})-", wheel).group(1)
        python_exe_name = "python{}.{}".format(python_version[0], python_version[1:])
        check_call([python_exe_name, "-m", "venv", envdir])

        env_python = os.path.join(envdir, "bin/python3")

        site_packages = check_output([env_python, "-c", "import site; print(site.getsitepackages()[0])"]).decode("utf8").strip()

        shutil.copy(os.path.join(os.path.dirname(__file__), "autoload/pyston_lite_autoload.pth"), site_packages)

        # ext_name = self.get_ext_fullpath(ext.name)
        # os.rename(ext_name, ext_name + ".prebolt")
        install_extname = os.path.join(site_packages, os.path.basename(so))
        check_call(["../../build/bolt/bin/llvm-bolt", so + ".prebolt", "-instrument", "-instrumentation-file-append-pid", "-instrumentation-file=" + os.path.abspath(install_extname), "-o", install_extname, "-skip-funcs=_PyEval_EvalFrameDefault,_PyEval_EvalFrame_AOT_Interpreter.*"])


        PGO_TESTS_TO_SKIP = "test_posix test_asyncio test_cmd_line_script test_compiler test_concurrent_futures test_ctypes test_dbm test_dbm_dumb test_dbm_ndbm test_distutils test_ensurepip test_ftplib test_gdb test_httplib test_imaplib test_ioctl test_linuxaudiodev test_multiprocessing test_nntplib test_ossaudiodev test_poplib test_pydoc test_signal test_socket test_socketserver test_ssl test_subprocess test_sundry test_thread test_threaded_import test_threadedtempfile test_threading test_threading_local test_threadsignals test_venv test_zipimport_support test_code test_capi test_multiprocessing_forkserver test_multiprocessing_spawn test_multiprocessing_fork".split()
        subprocess.call([env_python, os.path.join(os.path.dirname(__file__), "../../Lib/test/regrtest.py"), "-j0", "-unone,decimal", "-x"] + PGO_TESTS_TO_SKIP)

        check_call(["../../build/bolt/bin/merge-fdata"] + glob.glob(install_extname + ".*.fdata"), stdout=open(install_extname + ".fdata", "wb"))

        check_call(["../../build/bolt/bin/llvm-bolt", so + ".prebolt", "-o", so, "-data=" + install_extname + ".fdata", "-update-debug-sections", "-reorder-blocks=cache+", "-reorder-functions=hfsort+", "-split-functions=3", "-icf=1", "-inline-all", "-split-eh", "-reorder-functions-use-hot-size", "-peepholes=all", "-jump-tables=aggressive", "-inline-ap", "-indirect-call-promotion=all", "-dyno-stats", "-use-gnu-stack", "-jump-tables=none", "-frame-opt=hot"])
        os.unlink(so + ".prebolt")

        check_call(["zip", os.path.abspath(wheel), "-r"] + os.listdir(unzipdir), cwd=unzipdir)


if __name__ == "__main__":
    args = sys.argv[1:]

    for a in args:
        if "none-any" in a:
            continue
        bolt_wheel(a)

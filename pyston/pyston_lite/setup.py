from distutils.core import setup, Extension, Distribution
from distutils.command.build_ext import build_ext
from distutils import sysconfig
import glob
import os
import platform
import shutil
import subprocess
import sys
import tempfile

NOBOLT = "NOBOLT" in os.environ or sys.platform == "darwin" or platform.machine() == "aarch64"
NOLTO = "NOLTO" in os.environ or sys.platform == "darwin"
NOPGO = "NOPGO" in os.environ
BOLTFLAGS = "BOLTFLAGS" in os.environ

if not (3, 7) <= sys.version_info[:2] <= (3, 10):
    raise Exception("pyston-lite currently only targets Python 3.7, 3.8, 3.9 and 3.10")

def check_call(args, **kw):
    print("check_call", " ".join([repr(a) for a in args]), kw)
    return subprocess.check_call(args, **kw)

def check_output(args, **kw):
    print("check_output", " ".join([repr(a) for a in args]), kw)
    return subprocess.check_output(args, **kw)

def bolt_bin(name):
    if "PYSTON_USE_SYS_BINS" in os.environ:
        return name
    return os.path.join("../../build/bolt/bin", name)

class pyston_build_ext(build_ext):
    def build_extension(self, ext):
        extra_args = ext.extra_compile_args
        extra_link_args = ext.extra_link_args

        # I don't see a way to customize CFLAGS per source file, so reach into the
        # CCompiler object and wrap the _compile method to add our flags.
        orig_compile_func = self.compiler._compile
        def new_compile(obj, src, ext, cc_args, extra_postargs, pp_opts):
            # This file can't use lto due to the global registers:
            if "aot_ceval_jit_helper" in src:
                extra_postargs = extra_postargs + ["-fno-lto"]
            return orig_compile_func(obj, src, ext, cc_args, extra_postargs, pp_opts)
        self.compiler._compile = new_compile

        PGO_TESTS_TO_SKIP = "test_posix test_asyncio test_cmd_line_script test_compiler test_concurrent_futures test_ctypes test_dbm test_dbm_dumb test_dbm_ndbm test_distutils test_ensurepip test_ftplib test_gdb test_httplib test_imaplib test_ioctl test_linuxaudiodev test_multiprocessing test_nntplib test_ossaudiodev test_poplib test_pydoc test_signal test_socket test_socketserver test_ssl test_subprocess test_sundry test_thread test_threaded_import test_threadedtempfile test_threading test_threading_local test_threadsignals test_venv test_zipimport_support test_code test_capi test_multiprocessing_forkserver test_multiprocessing_spawn test_multiprocessing_fork".split()

        if NOPGO:
            super(pyston_build_ext, self).build_extension(ext)
        else:
            # Step 1, build with instrumentation:
            ext.extra_compile_args = extra_args + ["-fprofile-generate"]
            ext.extra_link_args = extra_link_args + ["-fprofile-generate"]
            super(pyston_build_ext, self).build_extension(ext)

            # Step 2, run pgo task:
            with tempfile.TemporaryDirectory() as dir:
                envdir = os.path.join(dir, "pgo_env")
                check_call([sys.executable, "-m", "venv", envdir])

                env_python = os.path.join(envdir, "bin/python3")

                site_packages = subprocess.check_output([env_python, "-c", "import site; print(site.getsitepackages()[0])"]).decode("utf8").strip()
                shutil.copy(self.get_ext_fullpath(ext.name), site_packages)
                shutil.copy(os.path.join(os.path.dirname(__file__), "autoload/pyston_lite_autoload.pth"), site_packages)

                check_call([env_python, "-c", "import sys; assert 'pyston_lite' in sys.modules"])

                parallel = "-j0"
                if subprocess.check_output(["uname", "-m"]).decode("utf8").strip() == "aarch64":
                    parallel = "-j1"

                subprocess.call([env_python, os.path.join(os.path.dirname(__file__), "../../Lib/test/regrtest.py"), parallel, "-unone,decimal", "-x"] + PGO_TESTS_TO_SKIP)

            # Step 3, recompile using profiling data:

            # Force recompilation:
            # (we could also try to clean things up but that seems more difficult)
            self.force = True

            ext.extra_compile_args = extra_args + ["-fprofile-use"]
            ext.extra_link_args = extra_link_args + ["-fprofile-use"]
            super(pyston_build_ext, self).build_extension(ext)

            ext.extra_compile_args = extra_args
            ext.extra_link_args = extra_link_args


        if not NOBOLT:
            with tempfile.TemporaryDirectory() as dir:
                envdir = os.path.join(dir, "bolt_env")
                check_call([sys.executable, "-m", "venv", envdir])

                env_python = os.path.join(envdir, "bin/python3")

                site_packages = check_output([env_python, "-c", "import site; print(site.getsitepackages()[0])"]).decode("utf8").strip()

                shutil.copy(os.path.join(os.path.dirname(__file__), "autoload/pyston_lite_autoload.pth"), site_packages)

                ext_name = self.get_ext_fullpath(ext.name)
                os.rename(ext_name, ext_name + ".prebolt")
                install_extname = os.path.join(site_packages, os.path.basename(ext_name))
                check_call([bolt_bin("llvm-bolt"), ext_name + ".prebolt", "-instrument", "-instrumentation-file-append-pid", "-instrumentation-file=" + os.path.abspath(install_extname), "-o", install_extname, "-skip-funcs=_PyEval_EvalFrameDefault,_PyEval_EvalFrame_AOT_Interpreter.*"])


                subprocess.call([env_python, os.path.join(os.path.dirname(__file__), "../../Lib/test/regrtest.py"), "-j0", "-unone,decimal", "-x"] + PGO_TESTS_TO_SKIP)

                check_call([bolt_bin("merge-fdata")] + glob.glob(install_extname + ".*.fdata"), stdout=open(install_extname + ".fdata", "wb"))

                check_call([bolt_bin("llvm-bolt"), ext_name + ".prebolt", "-o", ext_name, "-data=" + install_extname + ".fdata", "-update-debug-sections", "-reorder-blocks=cache+", "-reorder-functions=hfsort+", "-split-functions=3", "-icf=1", "-inline-all", "-split-eh", "-reorder-functions-use-hot-size", "-peepholes=all", "-jump-tables=aggressive", "-inline-ap", "-indirect-call-promotion=all", "-dyno-stats", "-use-gnu-stack", "-jump-tables=none", "-frame-opt=hot"])
                os.unlink(ext_name + ".prebolt")

        del self.compiler._compile

    def run(self):
        subprocess.check_call(["../../pyston/tools/dynasm_preprocess.py", "aot_ceval_jit.c", "aot_ceval_jit.prep.c"])
        subprocess.check_call(["luajit", "../../pyston/LuaJIT/dynasm/dynasm.lua", "-o", "aot_ceval_jit.gen.c", "aot_ceval_jit.prep.c"])

        super(pyston_build_ext, self).run()

def get_cflags():
    flags = ["-std=gnu99", "-fno-semantic-interposition", "-specs=../tools/no-pie-compile.specs", "-Wno-unused-function"]
    # make sure we catch undeclared functions
    flags.append("-Werror=implicit-function-declaration")
    if not NOLTO:
        flags += ["-flto", "-fuse-linker-plugin", "-ffat-lto-objects", "-flto-partition=none"]
    if not NOBOLT or BOLTFLAGS:
        flags += ["-fno-reorder-blocks-and-partition"]
    return flags

def get_ldflags():
    flags = ["-fno-semantic-interposition", "-specs=../tools/no-pie-link.specs"]
    if not NOLTO:
        flags += ["-flto", "-fuse-linker-plugin", "-ffat-lto-objects", "-flto-partition=none"]
    if not NOBOLT or BOLTFLAGS:
        flags += ["-Wl,--emit-relocs"]
    return flags

ext = Extension(
        "pyston_lite",
        sources=["aot_ceval.c", "aot_ceval_jit.gen.c", "aot_ceval_jit_helper.c", "lib.c", "aot_lite.c"],
        include_dirs=["../../pyston/LuaJIT", os.path.join(sysconfig.get_python_inc(), "internal")],
        define_macros=[("PYSTON_LITE", None), ("PYSTON_SPEEDUPS", "1"), ("Py_BUILD_CORE", None), ("ENABLE_AOT", None), ("NO_DKVERSION", None)],
        extra_compile_args=get_cflags(),
        extra_link_args=get_ldflags(),
)

long_description = """
pyston-lite is the JIT part of [Pyston](https://github.com/pyston/pyston),
a faster implementation of Python. pyston-lite does not contain all of the
optimizations of full Pyston, but it is still 10-25% faster on many workloads.

pyston-lite is currently only available for Python 3.8
""".strip()

setup(name="pyston_lite",
      cmdclass={"build_ext":pyston_build_ext},
      version="2.3.4.2",
      description="A JIT for Python",
      author="The Pyston Team",
      url="https://www.github.com/pyston/pyston",
      ext_modules=[ext],
      long_description=long_description,
      long_description_content_type="text/markdown",
)

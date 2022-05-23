from distutils.core import setup, Extension, Distribution
from distutils.command.build_ext import build_ext
from distutils import sysconfig
import os
import shutil
import subprocess
import sys
import tempfile

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


        ext.extra_compile_args = extra_args + ["-fprofile-generate"]
        ext.extra_link_args = extra_link_args + ["-fprofile-generate"]
        super(pyston_build_ext, self).build_extension(ext)

        with tempfile.TemporaryDirectory() as dir:
            envdir = os.path.join(dir, "pgo_env")
            subprocess.check_call([sys.executable, "-m", "venv", envdir])

            env_python = os.path.join(envdir, "bin/python3")

            site_packages = subprocess.check_output([env_python, "-c", "import site; print(site.getsitepackages()[0])"]).decode("utf8").strip()
            shutil.copy(self.get_ext_fullpath(ext.name), site_packages)
            shutil.copy(os.path.join(os.path.dirname(__file__), "autoload/pyston_lite_autoload.pth"), site_packages)

            subprocess.check_call([env_python, "-c", "import sys; assert 'pyston_lite' in sys.modules"])

            subprocess.call([env_python, os.path.join(os.path.dirname(__file__), "../../Lib/test/regrtest.py"), "-unone,decimal", "-x", *"test_posix test_asyncio test_cmd_line_script test_compiler test_concurrent_futures test_ctypes test_dbm_dumb test_dbm_ndbm test_distutils test_ensurepip test_ftplib test_gdb test_httplib test_imaplib test_ioctl test_linuxaudiodev test_multiprocessing test_nntplib test_ossaudiodev test_poplib test_pydoc test_signal test_socket test_socketserver test_ssl test_subprocess test_sundry test_thread test_threaded_import test_threadedtempfile test_threading test_threading_local test_threadsignals test_venv test_zipimport_support test_code test_capi".split()])

        # Force recompilation:
        # (we could also try to clean things up but that seems more difficult)
        self.force = True

        ext.extra_compile_args = extra_args + ["-fprofile-use"]
        ext.extra_link_args = extra_link_args + ["-fprofile-use"]
        super(pyston_build_ext, self).build_extension(ext)

        ext.extra_compile_args = extra_args
        ext.extra_link_args = extra_link_args
        del self.compiler._compile

    def run(self):
        subprocess.check_call(["../../pyston/tools/dynasm_preprocess.py", "aot_ceval_jit.c", "aot_ceval_jit.prep.c"])
        subprocess.check_call(["luajit", "../../pyston/LuaJIT/dynasm/dynasm.lua", "-o", "aot_ceval_jit.gen.c", "aot_ceval_jit.prep.c"])

        super(pyston_build_ext, self).run()

ext = Extension(
        "pyston_lite",
        sources=["aot_ceval.c", "aot_ceval_jit.gen.c", "aot_ceval_jit_helper.c", "lib.c"],
        include_dirs=["../../pyston/LuaJIT", os.path.join(sysconfig.get_python_inc(), "internal")],
        define_macros=[("PYSTON_LITE", None), ("PYSTON_SPEEDUPS", "1"), ("Py_BUILD_CORE", None), ("ENABLE_AOT", None)],
        extra_compile_args=["-std=gnu99", "-flto", "-fuse-linker-plugin", "-ffat-lto-objects", "-flto-partition=none", "-fno-semantic-interposition"],
        extra_link_args=["-flto", "-fuse-linker-plugin", "-ffat-lto-objects", "-flto-partition=none", "-fno-semantic-interposition"],
)

setup(name="pyston_lite",
      cmdclass={"build_ext":pyston_build_ext},
      version="2.3.3.1",
      description="A JIT for Python",
      author="The Pyston Team",
      url="https://www.github.com/pyston/pyston",
      ext_modules=[ext],
)

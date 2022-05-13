from distutils.core import setup, Extension, Distribution
from distutils.command.build_ext import build_ext
from distutils import sysconfig
import os
import subprocess
import sys

class pyston_build_ext(build_ext):
    def run(self):
        subprocess.check_call(["../../pyston/tools/dynasm_preprocess.py", "aot_ceval_jit.c", "aot_ceval_jit.prep.c"])
        subprocess.check_call(["luajit", "../../pyston/LuaJIT/dynasm/dynasm.lua", "-o", "aot_ceval_jit.gen.c", "aot_ceval_jit.prep.c"])
        return super(pyston_build_ext, self).run()

ext = Extension(
        "pyston_lite",
        sources=["aot_ceval.c", "aot_ceval_jit.gen.c", "aot_ceval_jit_helper.c", "lib.c"],
        include_dirs=["../../pyston/LuaJIT", os.path.join(sysconfig.get_python_inc(), "internal")],
        define_macros=[("PYSTON_LITE", None), ("PYSTON_SPEEDUPS", "1"), ("Py_BUILD_CORE", None), ("ENABLE_AOT", None)],
        extra_compile_args=["-std=gnu99"],
)

setup(name="pyston_lite",
      cmdclass={"build_ext":pyston_build_ext},
      version="2.3.3.1",
      description="A JIT for Python",
      author="The Pyston Team",
      url="https://www.github.com/pyston/pyston",
      ext_modules=[ext],
)

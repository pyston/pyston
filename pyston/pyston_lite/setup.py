from distutils.core import setup, Extension, Distribution
from distutils import sysconfig
import os
import sys

class PystonLiteExtension(Extension):
    pass

ext = PystonLiteExtension(
        "pyston_lite",
        sources=["aot_ceval.c", "aot_ceval_jit.gen.c", "aot_ceval_jit_helper.c", "lib.c"],
        include_dirs=["../../pyston/LuaJIT", os.path.join(sysconfig.get_python_inc(), "internal")],
        define_macros=[("PYSTON_LITE", None), ("PYSTON_SPEEDUPS", "1"), ("Py_BUILD_CORE", None), ("ENABLE_AOT", None)],
        extra_compile_args=["-std=gnu99"],
        headers=["dict-common.h"]
)

setup(name="pyston_lite",
      version="0.1",
      description="A JIT for Python",
      author="The Pyston Team",
      url="https://www.github.com/pyston/pyston",
      ext_modules=[ext],
)

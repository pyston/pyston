from distutils.core import setup, Extension, Distribution
from distutils import sysconfig
import os
import sys

VERSION = "2.3.3.1"
setup(name="pyston_lite_autoload",
      version=VERSION,
      description="Automatically loads and enables pyston_lite",
      author="The Pyston Team",
      url="https://www.github.com/pyston/pyston",
      data_files=[(sysconfig.get_python_lib(), ["pyston_lite_autoload.pth"])],
      install_requires=["pyston_lite==" + VERSION],
)

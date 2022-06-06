from distutils.command.install import install
from distutils.core import setup, Extension, Distribution
from distutils import sysconfig
import os
import sys

# From https://github.com/pypa/setuptools/blob/780cae233b51aa6b93b25e35538f496480bae537/setup.py
class install_with_pth(install):
    """
    Custom install command to install a .pth file for distutils patching.
    This hack is necessary because there's no standard way to install behavior
    on startup (and it's debatable if there should be one). This hack (ab)uses
    the `extra_path` behavior in Setuptools to install a `.pth` file with
    implicit behavior on startup to give higher precedence to the local version
    of `distutils` over the version from the standard library.
    Please do not replicate this behavior.
    """

    _pth_name = 'pyston_lite_autoload'
    _pth_contents = '''import os, sys; exec("""("DISABLE_PYSTON" not in os.environ) and __import__("pyston_lite").enable()""")'''

    def initialize_options(self):
        install.initialize_options(self)
        self.extra_path = self._pth_name, self._pth_contents

    def finalize_options(self):
        install.finalize_options(self)
        self._restore_install_lib()

    def _restore_install_lib(self):
        """
        Undo secondary effect of `extra_path` adding to `install_lib`
        """
        suffix = os.path.relpath(self.install_lib, self.install_libbase)

        if suffix.strip() == self._pth_contents.strip():
            self.install_lib = self.install_libbase

VERSION = "2.3.4.1"
setup(name="pyston_lite_autoload",
      cmdclass={"install": install_with_pth},
      version=VERSION,
      description="Automatically loads and enables pyston_lite",
      author="The Pyston Team",
      url="https://www.github.com/pyston/pyston",
      install_requires=["pyston_lite==" + VERSION],
)

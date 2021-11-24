import json
import os
import pickle
import re
import subprocess
import sys

_feedstock_overrides = {
    "typing-extensions": "typing_extensions",
    "py-lief": "lief",
    "matplotlib-base": "matplotlib",
    "g-ir-build-tools": "gobject-introspection",
    "g-ir-host-tools": "gobject-introspection",
    "libgirepository": "gobject-introspection",
    "gst-plugins-good": "gstreamer",
    "postgresql-plpython": "postgresql",
    "pyqt5-sip": "pyqt",
    "pyqt-impl": "pyqt",
    "pyqtwebengine": "pyqt",
    "pyqtchart": "pyqt",
    "argon2-cffi": "argon2_cffi",
    "atk-1.0": "atk",
    "pybind11-abi": "pybind11",
    "pybind11-global": "pybind11",
    "poppler-qt": "poppler",
    "cross-r-base": "r-base",
    "tensorflow-base": "tensorflow",
    "tensorflow-estimator": "tensorflow",
    "libtensorflow": "tensorflow",
    "libtensorflow_cc": "tensorflow",
    "llvm-openmp": "openmp",
    "ld_impl_linux-64": "binutils",
    "binutils_impl_linux-64": "binutils",
    "pytorch": "pytorch-cpu",
    "pytorch-gpu": "pytorch-cpu",
    "proj": "proj.4",
    "mysql-libs": "mysql",
    "mysql-server": "mysql",
    "mysql-client": "mysql",
    "mysql-devel": "mysql",
    "mysql-common": "mysql",
    "mysql-router": "mysql",
    "reproc-cpp": "reproc",
    "tbb4py": "tbb",
    "libopencv": "opencv",
    "py-opencv": "opencv",
    "libxgboost": "xgboost",
    "py-xgboost": "xgboost",
    "py-xgboost-cpu": "xgboost",
    "_py-xgboost-mutex": "xgboost",
    "_r-xgboost-mutex": "xgboost",
    "brotli-bin": "brotli",
    "llvm": "llvmdev",
    "llvm-tools": "llvmdev",
    "libblas": "blas",
    "libcblas": "blas",
    "liblapack": "blas",
    "liblapacke": "blas",
    "blas-devel": "blas",
    "zstd-static": "zstd",
    "clang": "clangdev",
    "libclang-cpp": "clangdev",
    "libclang": "clangdev",
    "clang-9": "clangdev",
    "clang-11": "clangdev",
    "clang-12": "clangdev",
    "clangxx": "clangdev",
    "clang-format": "clangdev",
    "clang-format-12": "clangdev",
    "clang-tools": "clangdev",
    "clang_osx-64": "clangdev",
    "clangxx_osx-64": "clangdev",
    "c-ares-static": "c-ares",
    "tbb-devel": "tbb",
    "ucx": "ucx-split",
    "ucx-proc": "ucx-split",
    "arrow-cpp-proc": "arrow-cpp",
    "pyarrow": "arrow-cpp",
    "pyarrow-tests": "arrow-cpp",
    "proj4": "proj.4",
    "dal-devel": "dal",
    "cctools": "cctools-and-ld64",
    "cctools_osx-64": "cctools-and-ld64",
    "ld64": "cctools-and-ld64",
    "ld64_osx-64": "cctools-and-ld64",
    "kubernetes-client": "kubernetes",
    "kubernetes-node": "kubernetes",
    "kubernetes-server": "kubernetes",
    "ray-all": "ray-packages",
    "ray-core": "ray-packages",
    "ray-default": "ray-packages",
    "ray-autoscaler": "ray-packages",
    "ray-dashboard": "ray-packages",
    "ray-debug": "ray-packages",
    "ray-k8s": "ray-packages",
    "ray-rllib": "ray-packages",
    "c-compiler": "compilers",
    "cxx-compiler": "compilers",
    "fortran-compiler": "compilers",
    "faiss-proc": "faiss-split",
    "faiss": "faiss-split",
    "faiss-cpu": "faiss-split",
    "faiss-gpu": "faiss-split",
    "libfaiss": "faiss-split",
    "libfaiss-avx2": "faiss-split",
    "apache-airflow": "airflow",
    "ptscotch": "scotch",
    "brotli-python": "brotli",
    "jupyterhub-base": "jupyterhub",
    "libthrift": "thrift-cpp",
    "thrift-compiler": "thrift-cpp",
    "cvxpy-base": "cvxpy",
    "cf-units": "cf_units",
    "gmock": "gtest",
    "importlib-metadata": "importlib_metadata",
}

def getFeedstockName(pkg):
    if pkg.startswith("airflow"):
        return "airflow"
    if pkg.startswith("mumps"):
        return "mumps"
    if pkg.startswith("gnuradio"):
        return "gnuradio"
    pkg = _feedstock_overrides.get(pkg, pkg)
    pkg = pkg.replace("_linux-64", "")
    return pkg

def getBuildRequirements(pkg):
    reponame = getFeedstockName(pkg) + "-feedstock"

    if not os.path.exists(reponame):
        subprocess.check_call(["git", "clone", "https://github.com/conda-forge/" + reponame], stdin=open("/dev/null"))

    with open(reponame + "/recipe/meta.yaml") as f:
        s = f.read()

    strings = re.findall("^ *- ([^><={\n#]+)", s, re.M)
    r = list(sorted(set([s.strip() for s in strings])))

    # ipython depends on ipykernel depends on ipython, but ipykernel depends on ipython more
    if pkg == "ipython":
        if "ipykernel" in r:
            r.remove("ipykernel")

    if pkg == "ipykernel":
        if "ipyparallel" in r:
            r.remove("ipyparallel")

    # This is spurious:
    if pkg == "jupyter_core":
        if "jupyter" in r:
            r.remove("jupyter")

    # These "downstream"s are picked up as a dependency; remove it
    if getFeedstockName(pkg) == "gobject-introspection":
        if "pygobject" in r:
            r.remove("pygobject")
    if getFeedstockName(pkg) == "cfitsio":
        if "gdal" in r:
            r.remove("gdal")

    # This is a run-constrained line:
    if pkg == "numba" and "cudatoolkit" in r:
        r.remove("cudatoolkit")

    if pkg in r:
        r.remove(pkg)

    return r

verbose = 0
_depends_on_python = {"python": True}
feedstock_order = []

packages_by_name = {}
noarch_packages = set()

feedstock_dependencies = {}

def getDependencies(pkg):
    """
    Returns the immediate package dependencies of a given package
    """

    if pkg not in packages_by_name:
        if verbose:
            print(repr(pkg), "is not a package we know about")
        return ()

    # We can't tell the difference between packages that build python extensions
    # vs packages that just contain python scripts as part of their build process.
    # So manually blacklist some of the latter type:
    for pattern in ("lib", "gcc", "gxx", "mkl", "glib", "gfortran", "dal(|-devel)$", "r-", "go-"):
        if re.match(pattern, pkg):
            return ()
    if getFeedstockName(pkg) in ("ninja", "krb5", "conda-build", "llvmdev", "hcc", "clangdev", "conda", "binutils", "cairo", "jack", "gstreamer", "cyrus-sasl", "hdf5", "openjdk", "bazel", "qt", "atk", "fftw", "yasm", "fribidi", "brunsli", "harfbuzz", "mpir", "gdk-pixbuf", "pango", "gtk2", "graphviz", "cudatoolkit"):
        return ()

    # This is a Python 2 library.
    # The simplistic yaml parsing ignores the "# [py27]" directive
    if pkg == "futures":
        return ()

    dependencies = set([d.split()[0] for d in packages_by_name[pkg]['depends']])
    if pkg not in noarch_packages and pkg not in ("python_abi", "certifi", "setuptools"):
        if verbose:
            print(pkg, getBuildRequirements(pkg))
        dependencies.update(getBuildRequirements(pkg))

    return sorted(dependencies)

def getFeedstockDependencies(feedstock):
    """
    Returns a set of feedstocks that need to be built before the given feedstock can be built
    Includes transitive dependencies
    """

    return feedstock_dependencies[feedstock]

def _dependsOnPython(pkg):
    dependencies = getDependencies(pkg)

    if verbose:
        print(pkg, dependencies)

    r = False
    for d in dependencies:
        subdepends = dependsOnPython(d)
        r = subdepends or r
        if subdepends and verbose:
            print(pkg, "depends on", d)

    _depends_on_python[pkg] = r

    f_deps = feedstock_dependencies.setdefault(getFeedstockName(pkg), set())
    for d in dependencies:
        dfn = getFeedstockName(d)
        f_deps.add(dfn)
        f_deps.update(feedstock_dependencies.get(dfn, ()))

    if not r:
        return False

    if pkg in ("glib", "python_abi", "certifi", "setuptools"):
        return False

    if pkg in noarch_packages:
        if verbose:
            print(pkg, "is noarch")
    else:
        name = getFeedstockName(pkg)
        if name not in feedstock_order:
            if verbose:
                print(name)
            feedstock_order.append(name)

    return r

def dependsOnPython(pkg):
    if pkg not in _depends_on_python:
        if verbose:
            print("Analyzing", pkg)
        _depends_on_python[pkg] = False # for circular dependencies
        _depends_on_python[pkg] = _dependsOnPython(pkg)
    return _depends_on_python[pkg]

def versionIsAtLeast(v1, v2):
    def tryInt(s):
        try:
            return int(s)
        except ValueError:
            return -1
    t1 = tuple(map(tryInt, v1.split('.')))
    t2 = tuple(map(tryInt, v2.split('.')))
    return t1 >= t2

def getFeedstockOrder(targets):
    global packages_by_name, noarch_packages
    if not packages_by_name and not os.path.exists("repo.pkl"):
        repodata_fn = "repodata_condaforge_linux64.json"
        if not os.path.exists(repodata_fn):
            print("Downloading...")
            subprocess.check_call(["wget", "https://conda.anaconda.org/conda-forge/linux-64/repodata.json.bz2", "-O", repodata_fn + ".bz2"])
            subprocess.check_call(["bzip2", "-d", repodata_fn + ".bz2"])

        repodata_noarch_fn = "repodata_condaforge_noarch.json"
        if not os.path.exists(repodata_noarch_fn):
            print("Downloading...")
            subprocess.check_call(["wget", "https://conda.anaconda.org/conda-forge/noarch/repodata.json.bz2", "-O", repodata_noarch_fn + ".bz2"])
            subprocess.check_call(["bzip2", "-d", repodata_noarch_fn + ".bz2"])

        if verbose:
            print("Loading...")
        data = json.load(open(repodata_fn))
        data_noarch = json.load(open(repodata_noarch_fn))

        for k, v in data["packages"].items():
            packages_by_name[v['name']] = v

        for k, v in data_noarch["packages"].items():
            if "pypy" in k:
                continue

            binary_package = packages_by_name.get(v['name'], None)
            if not binary_package or versionIsAtLeast(v['version'], binary_package['version']):
                packages_by_name[v['name']] = v
                noarch_packages.add(v['name'])

        pickle.dump((packages_by_name, noarch_packages), open("_repo.pkl", "wb"))
        os.rename("_repo.pkl", "repo.pkl")

    packages_by_name, noarch_packages = pickle.load(open("repo.pkl", "rb"))

    if verbose:
        print("Analyzing...")
    for name in targets:
        dependsOnPython(name)

    return feedstock_order

if __name__ == "__main__":
    targets = sys.argv[1:]

    if "-v" in targets:
        verbose = True
        targets.remove("-v")

    if not targets:
        raise Exception("Please pass targets on command line, or '-' for stdin")

    if targets == ['-']:
        targets = (l.strip() for l in sys.stdin)

    for feedstock in getFeedstockOrder(targets):
        print(feedstock)

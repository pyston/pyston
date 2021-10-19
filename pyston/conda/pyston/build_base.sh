#!/bin/sh
set -eux

# pyston should not compile llvm and bolt but instead use the conda packages
export PYSTON_USE_SYS_BINS=1

# the conda compiler is named 'x86_64-conda-linux-gnu-cc' but cpython compares
# the name to *gcc*, *clang* in the configure file - so we have to use the real name.
# Code mostly copied from the cpython recipe.
AR=$(basename "${AR}")
CC=$(basename "${GCC}")
CXX=$(basename "${CXX}")
RANLIB=$(basename "${RANLIB}")
READELF=$(basename "${READELF}")

# overwrite default conda build flags else the bolt instrumented binary will not work
CFLAGS="-isystem ${PREFIX}/include"
LDFLAGS="-Wl,-rpath,${PREFIX}/lib -Wl,-rpath-link,${PREFIX}/lib -L${PREFIX}/lib"
CPPFLAGS="-isystem ${PREFIX}/include"

# without this line we can't find zlib and co..
CPPFLAGS=${CPPFLAGS}" -I${PREFIX}/include"

rm -rf build

if [ "${PYSTON_UNOPT_BUILD}" = "1" ]; then
    make -j`nproc` unopt
    make -j`nproc` cpython_testsuite
    OUTDIR=${SRC_DIR}/build/unopt_install/usr
    PYSTON=${OUTDIR}/bin/python3
else
    RELEASE_PREFIX=${PREFIX} make -j`nproc` release
    RELEASE_PREFIX=${PREFIX} make -j`nproc` cpython_testsuite_release
    OUTDIR=${SRC_DIR}/build/release_install${PREFIX}
    PYSTON=${OUTDIR}/bin/python3.bolt
fi

cp $PYSTON ${PREFIX}/bin/python${PYTHON_VERSION2}-pyston${PYSTON_VERSION2}
ln -s ${PREFIX}/bin/python${PYTHON_VERSION2}-pyston${PYSTON_VERSION2} ${PREFIX}/bin/pyston
ln -s ${PREFIX}/bin/python${PYTHON_VERSION2}-pyston${PYSTON_VERSION2} ${PREFIX}/bin/pyston3

cp -r ${OUTDIR}/include/* ${PREFIX}/include/
cp -r ${OUTDIR}/lib/* ${PREFIX}/lib/

# remove pip
rm -r ${PREFIX}/lib/python${PYTHON_VERSION2}-pyston${PYSTON_VERSION2}/site-packages/pip*

# remove pystons site-packages directory and replace it with a symlink to cpythons default site-packages directory
# we copy in our site-package/README.txt and package it to make sure the directory get's created.
mkdir -p ${PREFIX}/lib/python${PYTHON_VERSION2}/site-packages || true
cp ${PREFIX}/lib/python${PYTHON_VERSION2}-pyston${PYSTON_VERSION2}/site-packages/README.txt ${PREFIX}/lib/python${PYTHON_VERSION2}/site-packages
rm -r ${PREFIX}/lib/python${PYTHON_VERSION2}-pyston${PYSTON_VERSION2}/site-packages
ln -s ${PREFIX}/lib/python${PYTHON_VERSION2}/site-packages/ ${PREFIX}/lib/python${PYTHON_VERSION2}-pyston${PYSTON_VERSION2}/site-packages

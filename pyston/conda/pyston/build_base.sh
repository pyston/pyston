#!/bin/sh
set -eux

# pyston should not compile llvm and bolt but instead use the conda packages
export PYSTON_USE_SYS_BINS=1

# overwrite default conda build flags else the bolt instrumented binary will not work
CFLAGS="-isystem ${PREFIX}/include"
LDFLAGS="-Wl,-rpath,${PREFIX}/lib -Wl,-rpath-link,${PREFIX}/lib -L${PREFIX}/lib"
CPPFLAGS="-isystem ${PREFIX}/include"

# without this line we can't find zlib and co..
CPPFLAGS=${CPPFLAGS}" -I${PREFIX}/include"

make -j`nproc` pyston3

OUTDIR=$SRC_DIR/build/opt_install

cp $OUTDIR/usr/bin/python3.bolt ${PREFIX}/bin/python3.8-pyston2.3
ln -s ${PREFIX}/bin/python3.8-pyston2.3 ${PREFIX}/bin/pyston
ln -s ${PREFIX}/bin/python3.8-pyston2.3 ${PREFIX}/bin/pyston3

cp -r $OUTDIR/usr/include/* ${PREFIX}/include/
cp -r $OUTDIR/usr/lib/* ${PREFIX}/lib/

# remove pip
rm -r ${PREFIX}/lib/python3.8-pyston2.3/site-packages/pip*

# replace python
#ln -sf ${PREFIX}/bin/python3.8-pyston2.3 ${PREFIX}/bin/python
#ln -sf ${PREFIX}/bin/python3.8-pyston2.3 ${PREFIX}/bin/python3

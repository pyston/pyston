#/bin/sh

set -ex

cd $(dirname $0)

PYTHON=/opt/python/cp38-cp38/bin/python3

make -C ../LuaJIT -j`nproc`
make -C ../LuaJIT -j`nproc` install
ln -sf luajit-2.1.0-beta3 /usr/local/bin/luajit

# manylinux2014 doesn't come with a python3 symlink:
if ! $(which python3) ; then
    ln -s $PYTHON /usr/local/bin/
fi

$PYTHON -m pip wheel . --no-deps -w wheelhouse/

$PYTHON -m pip wheel autoload/ --no-deps -w wheelhouse/

rm -rf build pyston_lite.egg-info

for whl in wheelhouse/*.whl; do
    if [[ $whl != *"none-any"* ]]; then
        auditwheel repair $whl --plat $PLAT -w wheelhouse/
        rm $whl
    fi
done

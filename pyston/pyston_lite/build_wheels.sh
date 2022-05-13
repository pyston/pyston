#/bin/sh

cd $(dirname $0)

PYTHON=/opt/python/cp38-cp38/bin/python3

yum install -y luajit

# manylinux2014 doesn't come with a python3 symlink:
if ! $(which python3) ; then
    ln -s $PYTHON /usr/local/bin/
fi

$PYTHON -m pip wheel . --no-deps -w wheelhouse/

rm -rf build pyston_lite.egg-info

for whl in wheelhouse/*.whl; do
    auditwheel repair $whl --plat $PLAT -w wheelhouse/
    rm $whl
done

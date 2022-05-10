#/bin/sh

cd $(dirname $0)

PYTHON=/opt/python/cp38-cp38/bin/python

$PYTHON -m pip wheel . --no-deps -w wheelhouse/

for whl in wheelhouse/*.whl; do
    auditwheel repair $whl --plat $PLAT -w wheelhouse/
done

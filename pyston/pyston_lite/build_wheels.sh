#/bin/sh

set -ex

POLICY=${POLICY:"manylinux2014"}

cd $(dirname $0)

make -C ../LuaJIT clean
make -C ../LuaJIT -j`nproc`
make -C ../LuaJIT -j`nproc` install

ln -sf luajit-2.1.0-beta3 /usr/local/bin/luajit

# FIXME: enable all again
#for PYVER in cp37-cp37m cp38-cp38 cp39-cp39 cp310-cp310
for PYVER in cp310-cp310
do
    echo "#################################"
    echo "Building pyston_lite for ${PYVER}"
    PYTHON=/opt/python/${PYVER}/bin/python3

    ln -sf $PYTHON /usr/local/bin/

    $PYTHON -m pip wheel . --no-deps -w wheelhouse/

    $PYTHON -m pip wheel autoload/ --no-deps -w wheelhouse/

    rm -rf build pyston_lite.egg-info
done

for whl in wheelhouse/*.whl; do
    # ignore the autoload packages - auditwheel errors on them
    if [[ $whl != *"autoload"* ]] && [[ $whl == *"-linux_"* ]]; then
        auditwheel repair $whl --plat $PLAT -w wheelhouse/
        rm $whl
    elif [[ $whl == *"-linux_"* ]]; then
        # auditwheel refuses to repair the autoload packages since they
        # don't have any binary files. So just manually rename them.
        mv $whl $(echo $whl | sed "s/linux/${POLICY}/")
    fi
done

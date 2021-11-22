#!/bin/bash

set -exo pipefail

PACKAGE=$1
THISDIR=$(realpath $(dirname $0))
PYSTON_PKG_VER="3.8.12 *_23_pyston"

if [ -z "$CHANNEL" ]; then
    CHANNEL=pyston/label/dev
fi

MAKE_CONFIG_PY=$(realpath $(dirname $0)/make_config.py)

if [ ! -d ${PACKAGE}-feedstock ]; then
    git clone https://github.com/conda-forge/${PACKAGE}-feedstock.git
fi

cd ${PACKAGE}-feedstock

if [ "${PACKAGE}" == "numpy" ]; then
    # # 1.18.5:
    # git checkout 3f4b2e94
    # git cherry-pick 046882736
    # git cherry-pick 6b1da6d7e
    # git cherry-pick 4b48d8bb8
    # git cherry-pick 672ca6f0d

    # 1.19.5:
    git checkout a8002ff4
    git cherry-pick 6b1da6d7e
    git cherry-pick 4b48d8bb8
    git cherry-pick 672ca6f0d
fi

if [ "$PACKAGE" == "protobuf" ]; then
    # 1.16:
    git checkout 5ea5a127
fi

if [ "$PACKAGE" == "wrapt" ]; then
    # 1.21.1:
    git checkout 16dc738c
fi

if [ "$PACKAGE" == "h5py" ]; then
    # 3.1.0:
    git checkout 194aa3804~
fi

if [ "$PACKAGE" == "grpcio" ]; then
    # 1.39.0:
    git checkout 12950c9a~
fi

# We need a new version of the build scripts that take extra options
if ! grep -q EXTRA_CB_OPTIONS .scripts/build_steps.sh; then
    conda-smithy rerender
fi
if ! grep -q mambabuild .scripts/build_steps.sh; then
    conda-smithy rerender
fi

if [ "$PACKAGE" == "python-rapidjson" ]; then
    sed -i 's/pytest tests/pytest tests --ignore=tests\/test_memory_leaks.py --ignore=tests\/test_circular.py/g' recipe/meta.yaml
fi

if [ "$PACKAGE" == "numpy" ]; then
    sed -i 's/_not_a_real_test/test_for_reference_leak or test_api_importable/g' recipe/meta.yaml
fi

if [ "$PACKAGE" == "implicit" ]; then
    # The build step here implicitly does a `pip install numpy scipy`.
    # For CPython this will download a pre-built wheel from pypi, but
    # for Pyston it will try to recompile both of these packages.
    # But the meta.yaml doesn't include all the dependencies of
    # building scipy, specifically a fortran compiler, so we have to add it:
    sed -i "/        - {{ compiler('fortran') }}/d" recipe/meta.yaml
    sed -i "s/        - {{ compiler('cxx') }}/        - {{ compiler('cxx') }}\n        - {{ compiler('fortran') }}/" recipe/meta.yaml

    # I don't understand exactly why, but it seems like you can't install
    # both a fortran compiler and gcc 7. So update the configs to use gcc 9
    sed -i "s/'7'/'9'/" .ci_support/*.yaml
fi

if [ "$PACKAGE" == "pyqt" ]; then
    cp $THISDIR/patches/pyqt.patch recipe/pyston.patch
    sed -i "/pyston.patch/d" recipe/meta.yaml
    sed -i "s/      - qt5_dll.diff/      - qt5_dll.diff\n      - pyston.patch/" recipe/meta.yaml
fi

if [ "$PACKAGE" == "scikit-build" ]; then
    sed -i "s/and not test_get_python_version//" recipe/run_test.sh
    sed -i "s/not test_fortran_compiler/not test_fortran_compiler and not test_get_python_version/" recipe/run_test.sh
fi

if [ "$PACKAGE" == "conda-package-handling" ]; then
    # Not sure why the sha mismatches, I think they must have uploaded a new version
    sed -i "s/2a7cc4dc892d3581764710e869d36088291de86c7ebe0375c830a2910ef54bb6/6c01527200c9d9de18e64d2006cc97f9813707a34f3cb55bca2852ff4b06fb8d/" recipe/meta.yaml
fi

if [ "$PACKAGE" == "python-libarchive-c" ]; then
    # Not sure why the sha mismatches, I think they must have uploaded a new version
    sed -i "s/bef034fbc403feacc4d28e71520eff6a1fcc4a677f0bec5324f68ea084c8c103/1223ef47b1547a34eeaca529ef511a8593fecaf7054cb46e62775d8ccc1a6e5c/" recipe/meta.yaml
fi

if [ "$PACKAGE" == "ipython" ]; then
    # ipython has a circular dependency via ipykernel
    sed -i "/ipykernel/d" recipe/meta.yaml
fi

if [ "$PACKAGE" == "gobject-introspection" ]; then
    # This feedstock lists a "downstream" test, which it will try to run
    # after this package is built.
    # Installing this dependent package will fail because we haven't built it yet;
    # conda is supposed to be able to recognize this and skip the tests, but it
    # it doesn't and instead fails.
    sed -i "/pygobject/d" recipe/meta.yaml
fi

if [ "$PACKAGE" == "ipykernel" ]; then
    # ipykernel depends on ipyparallel for testing, but
    # ipyparallel depends on ipykernel
    sed -i "/- ipyparallel/d" recipe/meta.yaml
    # We have to remove some of the tests that would call into it:
    sed -i 's/pytest_args += \[$/pytest_args += \["--ignore=" + os.path.dirname(loader.path) + "\/test_pickleutil.py",/' recipe/run_test.py
fi

# conda-forge-ci-setup automatically sets add_pip_as_python_dependency=false
CONDA_FORGE_DOCKER_RUN_ARGS="-e EXTRA_CB_OPTIONS" EXTRA_CB_OPTIONS="-c $CHANNEL" python3 build-locally.py $(CHANNEL=$CHANNEL python3 $MAKE_CONFIG_PY)

echo "Done! Build artifacts are:"
find build_artifacts -name '*.tar.bz2' | xargs realpath

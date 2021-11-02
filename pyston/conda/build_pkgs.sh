#!/bin/bash
set -eux

PYSTON_PKG_VER="3.8.12 *_23_pyston"
OUTPUT_DIR=${PWD}/release/conda_pkgs

if [ -d $OUTPUT_DIR ]
then
    echo "Directory $OUTPUT_DIR already exists";
    exit 1
fi
mkdir -p ${OUTPUT_DIR}

docker run -iv${PWD}:/pyston_dir:ro -v${OUTPUT_DIR}:/conda_pkgs --env PYSTON_UNOPT_BUILD continuumio/miniconda3 sh -s <<EOF
set -eux

apt-get update

# some cpython tests require /etc/protocols
apt-get install -y netbase

conda install conda-build -y
conda build pyston_dir/pyston/conda/compiler-rt -c pyston/label/dev --skip-existing
conda build pyston_dir/pyston/conda/bolt -c pyston/label/dev --skip-existing
conda build pyston_dir/pyston/conda/pyston -c pyston/label/dev
conda build pyston_dir/pyston/conda/python_abi
conda build pyston_dir/pyston/conda/python

conda install patch -y # required to apply the patches in some recipes

# This are the arch dependent pip dependencies.
# We set CONDA_ADD_PIP_AS_PYTHON_DEPENDENCY=0 to prevent the implicit dependency on pip when specifying python.
for pkg in certifi setuptools
do
    git clone https://github.com/AnacondaRecipes/\${pkg}-feedstock.git
    CONDA_ADD_PIP_AS_PYTHON_DEPENDENCY=0 conda build \${pkg}-feedstock/recipe --python="${PYSTON_PKG_VER}" --override-channels -c conda-forge --use-local
done

# build numpy 1.20.3 using openblas
git clone https://github.com/AnacondaRecipes/numpy-feedstock.git -b pbs_1.20.3_20210520T162213
# 'test_for_reference_leak' fails for pyston - disable it
sed -i 's/_not_a_real_test/test_for_reference_leak/g' numpy-feedstock/recipe/meta.yaml
conda build numpy-feedstock/ --python="${PYSTON_PKG_VER}" --override-channels -c conda-forge --use-local --extra-deps pyston --variants="{blas_impl: openblas, openblas: 0.3.3, c_compiler_version: 7.5.0, cxx_compiler_version: 7.5.0}"

for arch in noarch linux-64
do
    mkdir /conda_pkgs/\${arch}
    cp /opt/conda/conda-bld/\${arch}/*.tar.bz2 /conda_pkgs/\${arch}
done
chown -R $(id -u):$(id -g) /conda_pkgs/

EOF

#!/bin/bash
set -eux

PYSTON_PKG_VER="3.8.5 *_23_pyston"
OUT_DIR=${PWD}/release/conda_pkgs

mkdir -p ${OUT_DIR}

docker run -iv${PWD}:/pyston_dir:ro -v${OUT_DIR}:/conda_pkgs continuumio/miniconda3 sh -s <<EOF
set -eux
conda install conda-build -y
conda build pyston_dir/pyston/conda/compiler-rt
conda build pyston_dir/pyston/conda/bolt
conda build pyston_dir/pyston/conda/pyston
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

for arch in noarch linux-64
do
    mkdir /conda_pkgs/\${arch}
    cp /opt/conda/conda-bld/\${arch}/*.tar.bz2 /conda_pkgs/\${arch}
done

EOF

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
apt-get install -y netbase curl patch

conda install conda-build -y
conda build pyston_dir/pyston/conda/compiler-rt -c pyston/label/dev --skip-existing -c conda-forge --override-channels
conda build pyston_dir/pyston/conda/bolt -c pyston/label/dev --skip-existing -c conda-forge --override-channels
conda build pyston_dir/pyston/conda/pyston -c pyston/label/dev -c conda-forge --override-channels -m pyston_dir/pyston/conda/pyston/variants.yaml
conda build pyston_dir/pyston/conda/python_abi -c conda-forge --override-channels
conda build pyston_dir/pyston/conda/python -c conda-forge --override-channels

# This are the arch dependent pip dependencies.
for pkg in certifi setuptools
do
    git clone https://github.com/conda-forge/\${pkg}-feedstock.git
    CONDA_ADD_PIP_AS_PYTHON_DEPENDENCY=0 conda build \${pkg}-feedstock/recipe --python="${PYSTON_PKG_VER}" --override-channels -c conda-forge --use-local
done

for arch in noarch linux-64
do
    mkdir -p /conda_pkgs/\${arch}
    cp /opt/conda/conda-bld/\${arch}/*.tar.bz2 /conda_pkgs/\${arch} || true
done
chown -R $(id -u):$(id -g) /conda_pkgs/

EOF

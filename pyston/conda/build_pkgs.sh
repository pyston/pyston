#!/bin/bash
set -eux

if [ ! -d "pyston/conda" ]; then
    echo "Please run from the top level pyston/ directory"
    exit 1
fi

PYSTON_PKG_VER="3.8.12 *_23_pyston"
OUTPUT_DIR=${PWD}/release/conda_pkgs

if [ -d $OUTPUT_DIR ]
then
    echo "Directory $OUTPUT_DIR already exists";
    exit 1
fi
mkdir -p ${OUTPUT_DIR}

OPTIONAL_ARGS=
if [ "${1:-}" = "--ci-mode" ]
then
    # CI is setup to write core dumps into /cores also give the container a name for easy access
    OPTIONAL_ARGS="-iv/cores:/cores --name pyston_build"
fi

docker run -iv${PWD}:/pyston_dir:ro -v${OUTPUT_DIR}:/conda_pkgs ${OPTIONAL_ARGS} --env PYSTON_UNOPT_BUILD continuumio/miniconda3 sh -s <<EOF
set -eux

apt-get update

# some cpython tests require /etc/protocols
apt-get install -y netbase curl patch

if [ `uname -m` = "aarch64" ]; then
    apt-get install -y luajit  # we have to install luajit via apt because conda-forge does not provide it
fi


conda install conda-build -y
conda build pyston_dir/pyston/conda/bolt -c pyston --skip-existing -c conda-forge --override-channels
conda build pyston_dir/pyston/conda/pyston -c pyston -c conda-forge --override-channels -m pyston_dir/pyston/conda/pyston/variants.yaml
conda build pyston_dir/pyston/conda/python_abi -c conda-forge --override-channels
conda build pyston_dir/pyston/conda/python -c conda-forge --override-channels
conda build pyston_dir/pyston/conda/pyston_lite -c pyston -c conda-forge --override-channels

# This are the arch dependent pip dependencies.
for pkg in certifi setuptools
do
    git clone https://github.com/conda-forge/\${pkg}-feedstock.git
    CONDA_ADD_PIP_AS_PYTHON_DEPENDENCY=0 conda build \${pkg}-feedstock/recipe --python="${PYSTON_PKG_VER}" --override-channels -c conda-forge --use-local
done

for arch in noarch linux-64 linux-aarch64
do
    mkdir -p /conda_pkgs/\${arch}
    cp /opt/conda/conda-bld/\${arch}/*.tar.bz2 /conda_pkgs/\${arch} || true
done
chown -R $(id -u):$(id -g) /conda_pkgs/

EOF

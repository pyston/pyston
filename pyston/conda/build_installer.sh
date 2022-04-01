#!/bin/bash
set -eux

if [ ! -d "pyston/conda" ]; then
    echo "Please run from the top level pyston/ directory"
    exit 1
fi

OUTPUT_DIR=${PWD}/release/conda_pkgs

# build the installer
docker run -iv${PWD}:/pyston_dir:ro -v${OUTPUT_DIR}:/conda_pkgs continuumio/miniconda3 sh -s <<EOF
set -eux
conda install -y constructor -c conda-forge --override-channels
constructor --output-dir /conda_pkgs/ pyston_dir/pyston/conda/installer
chown -R $(id -u):$(id -g) /conda_pkgs/
EOF


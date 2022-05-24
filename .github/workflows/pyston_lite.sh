#!/bin/bash
set -eux

# workaround for setuptools 60
export SETUPTOOLS_USE_DISTUTILS=stdlib

export DEBIAN_FRONTEND=noninteractive

# install dependencies
sudo apt-get update

sudo --preserve-env=DEBIAN_FRONTEND apt-get install -y build-essential python3.8-full python3.8-dev libpython3.8-testsuite virtualenv luajit

sudo chown -R `whoami` /pyston_dir

cd /pyston_dir/pyston/pyston_lite

if [ -z ${NOBOLT+x} ]; then
    make -C ../.. -j$(nproc) bolt
fi

make test -j$(nproc)

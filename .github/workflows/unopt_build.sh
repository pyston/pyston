#!/bin/bash
set -eux

# workaround for setuptools 60
export SETUPTOOLS_USE_DISTUTILS=stdlib

export DEBIAN_FRONTEND=noninteractive

# install dependencies
sudo apt-get update
sudo --preserve-env=DEBIAN_FRONTEND apt-get install -y build-essential ninja-build git cmake clang llvm libssl-dev libsqlite3-dev luajit python3.8 zlib1g-dev virtualenv libjpeg-dev linux-tools-common linux-tools-generic
sudo --preserve-env=DEBIAN_FRONTEND apt-get install -y libwebp-dev libjpeg-dev python3.8-gdbm python3.8-tk python3.8-dev tk-dev libgdbm-dev libgdbm-compat-dev liblzma-dev libbz2-dev nginx time
sudo --preserve-env=DEBIAN_FRONTEND apt-get install -y llvm-dev

export PYSTON_USE_SYS_BINS=1 # build has to use system llvm and clang binaries

sudo chown -R `whoami` /pyston_dir
cd /pyston_dir

make unopt -j$(nproc)

# running the tests
git submodule update --init "pyston/test/*"
# have to install late because it will update llvm-10 to 13 or so
sudo --preserve-env=DEBIAN_FRONTEND apt-get install -y rustc

make test -j$(nproc)

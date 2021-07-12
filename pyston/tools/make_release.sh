#!/bin/bash
set -e
VERSION=2.3
OUTPUT_DIR=${PWD}/release/${VERSION}

if [ -d $OUTPUT_DIR ]
then
    echo "Directory $OUTPUT_DIR already exists";
    exit 1
fi
mkdir -p $OUTPUT_DIR

# clean repo just to be sure, should also speedup copying the repo while running docker build
make clean

for DIST in 16.04 18.04 20.04
do
    echo "Creating $DIST release"
    docker build -f pyston/Dockerfile.$DIST -t pyston-build:$DIST .
    docker run -iv${PWD}/release/$VERSION:/host-volume --rm pyston-build:$DIST sh -s <<EOF
set -ex
make package -j`nproc`
apt-get install -y patchelf
make pyston/build/release_env/bin/python3
pyston/build/release_env/bin/python3 pyston/tools/make_portable_dir.py pyston_${VERSION}_amd64.deb pyston_${VERSION}_${DIST}
chown -R $(id -u):$(id -g) pyston_${VERSION}_amd64.deb
chown -R $(id -u):$(id -g) pyston_${VERSION}_${DIST}
cp -ar pyston_${VERSION}_amd64.deb /host-volume/pyston_${VERSION}_${DIST}.deb
cp -ar pyston_${VERSION}_${DIST} /host-volume/pyston_${VERSION}_${DIST}
# create archive of portable dir
cd /host-volume/pyston_${VERSION}_${DIST}
tar -czf ../pyston_${VERSION}_${DIST}.tar.gz *
EOF
    docker image rm pyston-build:$DIST
done

echo "FINISHED: wrote release to $OUTPUT_DIR"

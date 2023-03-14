#!/bin/bash

set -eu

VERSION=2.3.5
SRC_DIR=`git rev-parse --show-toplevel`
OUTPUT_DIR=${PWD}/release/${VERSION}
ARCH=`dpkg --print-architecture`

PARALLEL=
RELEASES=
while [[ $# -gt 0 ]]; do
    case $1 in
        -j)
            PARALLEL=1
            shift
            ;;
        *)
            RELEASES="$RELEASES $1"
            shift
            ;;
    esac
done

if [ -z "$RELEASES" ]; then
    RELEASES="18.04 20.04 22.04"

    if [ -d $OUTPUT_DIR ]
    then
        echo "Directory $OUTPUT_DIR already exists";
        exit 1
    fi
fi

# Make sure we have enough space available to do the build.
# I haven't really tested these numbers, I think they're a bit lower
# than the actual requirement
if [ -z "$PARALLEL" ]; then
    # 1kb blocks:
    REQ_SPACE=20000000
else
    REQ_SPACE=60000000
fi

if [ $(df . | awk 'NR==2 {print $4}') -lt $REQ_SPACE ]; then
    echo "Not enough disk space available"
    exit 1
fi

# Setting this avoided a perf crash:
echo "Setting kptr_restrict=0..."
echo 0 | sudo tee /proc/sys/kernel/kptr_restrict

mkdir -p $OUTPUT_DIR


function make_release {
    DIST=$1

    echo "Creating $DIST release"
    docker build -f pyston/make_release_files/Dockerfile.$DIST -t pyston-build:$DIST pyston/make_release_files/ --no-cache
    docker run -iv${PWD}/release/$VERSION:/host-volume -v${SRC_DIR}:/src/src_dir_host:ro --rm --cap-add SYS_ADMIN pyston-build:$DIST sh -s <<EOF
set -ex

# make a copy of the source because it's mounted read only and we want to modify it
rsync -rl /src/src_dir_host/ /src/build/ --exclude build
cd /src/build
rm -rf build
git clean -fdx

ln -sf /usr/lib/linux-tools/*/perf /usr/bin/perf
make package -j`nproc`
make build/release_env/bin/python3
build/release_env/bin/python3 pyston/tools/make_portable_dir.py pyston_${VERSION}_${ARCH}.deb pyston_${VERSION}
chown -R $(id -u):$(id -g) pyston_${VERSION}_${ARCH}.deb
chown -R $(id -u):$(id -g) pyston_${VERSION}
cp -ar pyston_${VERSION}_${ARCH}.deb /host-volume/pyston_${VERSION}_${DIST}_${ARCH}.deb
cp -ar pyston_${VERSION} /host-volume/pyston_${VERSION}_${DIST}_${ARCH}
# create archive of portable dir
tar -czf /host-volume/pyston_${VERSION}_${DIST}_${ARCH}.tar.gz pyston_${VERSION}
EOF
    docker image rm pyston-build:$DIST
}

for DIST in $RELEASES
do
    if [ -n "$PARALLEL" ]; then
        make_release $DIST &
    else
        make_release $DIST
    fi
done
wait

ln -sf --relative $OUTPUT_DIR/pyston_${VERSION}_18.04_${ARCH}.tar.gz $OUTPUT_DIR/pyston_${VERSION}_portable_${ARCH}.tar.gz

echo "FINISHED: wrote release to $OUTPUT_DIR"

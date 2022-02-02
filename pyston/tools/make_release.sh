#!/bin/bash

set -eu

VERSION=2.3.2
OUTPUT_DIR=${PWD}/release/${VERSION}

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
    RELEASES="18.04 20.04"

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
    docker build -f pyston/Dockerfile.$DIST -t pyston-build:$DIST . --no-cache
    docker run -iv${PWD}/release/$VERSION:/host-volume --rm --cap-add SYS_ADMIN pyston-build:$DIST sh -s <<EOF
set -ex
ln -sf /usr/lib/linux-tools/*/perf /usr/bin/perf
make package -j`nproc`
make build/release_env/bin/python3
build/release_env/bin/python3 pyston/tools/make_portable_dir.py pyston_${VERSION}_amd64.deb pyston_${VERSION}
chown -R $(id -u):$(id -g) pyston_${VERSION}_amd64.deb
chown -R $(id -u):$(id -g) pyston_${VERSION}
cp -ar pyston_${VERSION}_amd64.deb /host-volume/pyston_${VERSION}_${DIST}.deb
cp -ar pyston_${VERSION} /host-volume/pyston_${VERSION}_${DIST}
# create archive of portable dir
tar -czf /host-volume/pyston_${VERSION}_${DIST}.tar.gz pyston_${VERSION}
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

ln -sf --relative $OUTPUT_DIR/pyston_${VERSION}_18.04.tar.gz $OUTPUT_DIR/pyston_${VERSION}_portable.tar.gz

echo "FINISHED: wrote release to $OUTPUT_DIR"

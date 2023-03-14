#!/bin/bash
set -e
VERSION=2.3.5
INPUT_DIR=${PWD}/release/${VERSION}
OUTPUT_DIR=`realpath ${INPUT_DIR}/bench`
ARCH=`dpkg --print-architecture`

if [ -d $OUTPUT_DIR ]
then
    echo "Directory $OUTPUT_DIR already exists";
    exit 1
fi
mkdir -p $OUTPUT_DIR

for DIST in 18.04 20.04 22.04
do
    echo "Benchmarking $DIST release"
    # mount input dir readonly to make sure we are not accidently modifying it
    docker run -iv${INPUT_DIR}:/host-volume-in:ro -iv${OUTPUT_DIR}:/host-volume-out --rm ubuntu:$DIST sh -s <<EOF
set -ex
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get upgrade -y
apt-get install -y build-essential git time libffi-dev nginx
# pillow deps:
apt-get install -y libtiff5-dev libjpeg8-dev libopenjp2-7-dev zlib1g-dev libfreetype6-dev liblcms2-dev libwebp-dev tcl8.6-dev tk8.6-dev python3-tk libharfbuzz-dev libfribidi-dev libxcb1-dev

git clone https://github.com/pyston/python-macrobenchmarks.git
# disble pytorch test
sed -i 's/pytorch_alexnet_inference//g' python-macrobenchmarks/run_all.sh

# pyston deb package
apt-get install -y /host-volume-in/pyston_${VERSION}_${DIST}_${ARCH}.deb
pyston -mpip install pyperformance==1.0.1 virtualenv
pyston -mpyperformance run -f -o /host-volume-out/pyston_${VERSION}_${DIST}_${ARCH}.json
chown -R $(id -u):$(id -g) /host-volume-out/pyston_${VERSION}_${DIST}_${ARCH}.json

python-macrobenchmarks/run_all.sh /usr/bin/pyston
mv results /host-volume-out/results_pyston_${DIST}_${ARCH}
chown -R $(id -u):$(id -g) /host-volume-out/results_pyston_${DIST}_${ARCH}

# cpython
# some benchmarks don't run on python 3.5/3.6 so only run this benchmarks on 3.8 (20.04)
if [ "$DIST" = "20.04" ]; then
    apt-get install -y python3 python3-dev python3-pip python3-venv
    pyston -mpyperformance run -f -p /usr/bin/python3 -o /host-volume-out/cpython_${DIST}_${ARCH}.json
    chown -R $(id -u):$(id -g) /host-volume-out/cpython_${DIST}_${ARCH}.json
    pyston -mpyperformance compare -O table /host-volume-out/cpython_${DIST}_${ARCH}.json /host-volume-out/pyston_${VERSION}_${DIST}_${ARCH}.json > /host-volume-out/pyperformance_diff_${DIST}_${ARCH}.txt
    chown -R $(id -u):$(id -g) /host-volume-out/pyperformance_diff_${DIST}_${ARCH}.txt

    python-macrobenchmarks/run_all.sh /usr/bin/python3
    mv results /host-volume-out/results_cpython_${DIST}_${ARCH}
    chown -R $(id -u):$(id -g) /host-volume-out/results_cpython_${DIST}_${ARCH}
fi
EOF
done

echo "FINISHED: wrote benchmark results to $OUTPUT_DIR"

# display a summary of the benchmark results:
cat ${OUTPUT_DIR}/pyperformance_diff_20.04_${ARCH}.txt

for f in ${OUTPUT_DIR}/results_pyston_20.04_${ARCH}/*.out
do
    echo "benchmark: `basename $f`"
    for DIST in 18.04 20.04 22.04
    do
        f_pyston=${f/20.04/$DIST}
        f_cpython=${f_pyston/results_pyston/results_cpython}
        t_pyston=`cat $f_pyston | grep 'User time' | cut -d' ' -f4`
        if [ -f "$f_cpython" ]; then
            t_cpython=`cat $f_cpython | grep 'User time' | cut -d' ' -f4`
            t_speedup=`python3 -c "print('%.2f' % (float($t_cpython) / float($t_pyston)))"`
            echo "  $DIST# pyston: $t_pyston cpython: $t_cpython speedup: $t_speedup"
	else
            echo "  $DIST# pyston: $t_pyston"
        fi
    done
done


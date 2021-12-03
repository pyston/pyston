set -euxo pipefail

PACKAGE=$1

which anaconda

CHANNEL=pyston CI=1 bash $(dirname $0)/build_feedstock.sh $PACKAGE
anaconda upload -u pyston $(find $PACKAGE-feedstock/build_artifacts/ -name '*.tar.bz2' | grep -v broken | grep -v src_cache)
rm -rf $PACKAGE-feedstock/build_artifacts

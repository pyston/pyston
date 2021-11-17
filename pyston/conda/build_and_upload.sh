set -euxo pipefail

PACKAGE=$1

which anaconda

CHANNEL=kmod/label/dev CI=1 bash $(dirname $0)/build_feedstock.sh $PACKAGE
anaconda upload -u kmod --label dev $(find $PACKAGE-feedstock/build_artifacts/ -name '*.tar.bz2' | grep -v broken | grep -v src_cache)
rm -rf $PACKAGE-feedstock/build_artifacts

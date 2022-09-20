#!/bin/sh
# this script will not work if run outside of a CI per default
# because it will not find all required python versions and can't install
# the missing ones :(. On Mac only one Python can be installed system wide at a time.
# set 'CI=1' when running the script to remove the CI check and let the script overwrite your system wide python...

set -ex

ENV_DIR=`mktemp -d`

python3.8 -m venv $ENV_DIR
${ENV_DIR}/bin/pip install cibuildwheel

export CC=gcc-11
export CIBW_BUILD="cp37-* cp38-* cp39-* cp310-*"
export CIBW_SKIP="*_i686 *musllinux*"
export CIBW_BUILD_VERBOSITY=2
export CIBW_ENVIRONMENT="MACOSX_DEPLOYMENT_TARGET=10.16"

${ENV_DIR}/bin/cibuildwheel --platform macos autoload/

CIBW_TEST_COMMAND="pip install pyston_lite_autoload --no-index --find-links file://`pwd`/wheelhouse/ && python -m test -j0 -x test_code test_distutils test_ensurepip test_minidom test_site test_xml_etree test_xml_etree_c test_capi test_bdb test_c_locale_coercion test_ctypes test_importlib test_ssl test_sysconfig test_platform test___all__ test_sundry test_sys" \
${ENV_DIR}/bin/cibuildwheel --platform macos 

rm -rf ${ENV_DIR}


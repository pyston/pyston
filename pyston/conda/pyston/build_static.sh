set -eux

NAME=libpython${PYTHON_VERSION2}-pyston${PYSTON_VERSION2}.a

if [ "${PYSTON_UNOPT_BUILD}" = "1" ]; then
    BUILDNAME=unopt
else
    BUILDNAME=release
fi

cp $(find build/${BUILDNAME}_install/ -name $NAME | grep -v config) ${PREFIX}/lib/

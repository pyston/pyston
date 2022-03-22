set -eux

VER=${PYTHON_VERSION2}-pyston${PYSTON_VERSION2}
NAME=libpython${VER}.a
ARCH=`uname -m`

if [ "${PYSTON_UNOPT_BUILD}" = "1" ]; then
    BUILDNAME=unopt
else
    BUILDNAME=release
fi

cp $(find build/${BUILDNAME}_install/ -name $NAME | grep -v config) ${PREFIX}/lib/

# Size reductions:
chmod +w ${PREFIX}/lib/${NAME}
${STRIP} -S ${PREFIX}/lib/${NAME}

# Strip LTO sections because they depent on exact gcc version:
# we use fat objects which means only the LTO part is stripped the normal
# object files are still present and can be used for non lto linking
# (but the resulting binary will be slower).
${STRIP} -R ".gnu.lto_*" -R ".gnu.debuglto_*" -N "__gnu_lto_v1" ${PREFIX}/lib/${NAME}

# generate a symlink to the libpython.a file because we removed it in build_base.sh
ln -s ../../${NAME} ${PREFIX}/lib/python${VER}/config-${VER}-${ARCH}-linux-gnu/${NAME}

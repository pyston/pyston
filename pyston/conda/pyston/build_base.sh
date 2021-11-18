#!/bin/bash
set -eux

# pyston should not compile llvm and bolt but instead use the conda packages
export PYSTON_USE_SYS_BINS=1

rm -rf build


# BOLT seems to miscompile pyston when -fno-plt is passed - disable it
CFLAGS=$(echo "${CFLAGS}" | sed "s/-fno-plt//g")

_OPTIMIZED=yes
VERFULL=${PKG_VERSION}
VER=${PYTHON_VERSION2}-pyston${PYSTON_VERSION2}
VERNODOTS=${VER//./}
TCLTK_VER=${tk}

ABIFLAGS=
VERABI=${VER}

#########################################################################################
# Following code is mostly copied from the cpython recipe with some changes
# https://github.com/AnacondaRecipes/python-feedstock/blob/master-3.8/recipe/build_base.sh

# This is the mechanism by which we fall back to default gcc, but having it defined here
# would probably break the build by using incorrect settings and/or importing files that
# do not yet exist.
unset _PYTHON_SYSCONFIGDATA_NAME
unset _CONDA_PYTHON_SYSCONFIGDATA_NAME

# Remove bzip2's shared library if present,
# as we only want to link to it statically.
# This is important in cases where conda
# tries to update bzip2.
find "${PREFIX}/lib" -name "libbz2*${SHLIB_EXT}*" | xargs rm -fv {}

# Prevent lib/python${VER}/_sysconfigdata_*.py from ending up with full paths to these things
# in _build_env because _build_env will not get found during prefix replacement, only _h_env_placeh ...
AR=$(basename "${AR}")

# CC must contain the string 'gcc' or else distutils thinks it is on macOS and uses '-R' to set rpaths.
if [[ ${target_platform} == osx-* ]]; then
  CC=$(basename "${CC}")
else
  CC=$(basename "${GCC}")
fi
CXX=$(basename "${CXX}")
RANLIB=$(basename "${RANLIB}")
READELF=$(basename "${READELF}")

# Debian uses -O3 then resets it at the end to -O2 in _sysconfigdata.py
if [[ ${_OPTIMIZED} = yes ]]; then
  CPPFLAGS=$(echo "${CPPFLAGS}" | sed "s/-O2/-O3/g")
  CFLAGS=$(echo "${CFLAGS}" | sed "s/-O2/-O3/g")
  CXXFLAGS=$(echo "${CXXFLAGS}" | sed "s/-O2/-O3/g")
fi

declare -a LTO_CFLAGS=()

CPPFLAGS=${CPPFLAGS}" -I${PREFIX}/include"

re='^(.*)(-I[^ ]*)(.*)$'
if [[ ${CFLAGS} =~ $re ]]; then
  CFLAGS="${BASH_REMATCH[1]}${BASH_REMATCH[3]}"
fi

export CPPFLAGS CFLAGS CXXFLAGS LDFLAGS

# This causes setup.py to query the sysroot directories from the compiler, something which
# IMHO should be done by default anyway with a flag to disable it to workaround broken ones.
# Technically, setting _PYTHON_HOST_PLATFORM causes setup.py to consider it cross_compiling
if [[ -n ${HOST} ]]; then
  if [[ ${HOST} =~ .*darwin.* ]]; then
    # Even if BUILD is .*darwin.* you get better isolation by cross_compiling (no /usr/local)
    IFS='-' read -r host_arch host_os host_kernel <<<"${HOST}"
    export _PYTHON_HOST_PLATFORM=darwin-${host_arch}
  else
    IFS='-' read -r host_arch host_vendor host_os host_libc <<<"${HOST}"
    export _PYTHON_HOST_PLATFORM=${host_os}-${host_arch}
  fi
fi

if [[ ${target_platform} == osx-64 ]]; then
  export MACHDEP=darwin
  export ac_sys_system=Darwin
  export ac_sys_release=13.4.0
  export MACOSX_DEFAULT_ARCH=x86_64
  # TODO: check with LLVM 12 if the following hack is needed.
  # https://reviews.llvm.org/D76461 may have fixed the need for the following hack.
  echo '#!/bin/bash' > $BUILD_PREFIX/bin/$HOST-llvm-ar
  echo "$BUILD_PREFIX/bin/llvm-ar --format=darwin" '"$@"' >> $BUILD_PREFIX/bin/$HOST-llvm-ar
  chmod +x $BUILD_PREFIX/bin/$HOST-llvm-ar
  export ARCHFLAGS="-arch x86_64"
elif [[ ${target_platform} == osx-arm64 ]]; then
  export MACHDEP=darwin
  export ac_sys_system=Darwin
  export ac_sys_release=20.0.0
  export MACOSX_DEFAULT_ARCH=arm64
  echo '#!/bin/bash' > $BUILD_PREFIX/bin/$HOST-llvm-ar
  echo "$BUILD_PREFIX/bin/llvm-ar --format=darwin" '"$@"' >> $BUILD_PREFIX/bin/$HOST-llvm-ar
  chmod +x $BUILD_PREFIX/bin/$HOST-llvm-ar
  export ARCHFLAGS="-arch arm64"
  export CFLAGS="$CFLAGS $ARCHFLAGS"
elif [[ ${target_platform} == linux-* ]]; then
  export MACHDEP=linux
  export ac_sys_system=Linux
  export ac_sys_release=
fi

declare -a _common_configure_args
_common_configure_args+=(--build=${BUILD})
_common_configure_args+=(--host=${HOST})
_common_configure_args+=(--enable-ipv6)
_common_configure_args+=(--with-computed-gotos)
_common_configure_args+=(--with-system-ffi)
_common_configure_args+=(--enable-loadable-sqlite-extensions)
_common_configure_args+=(--with-openssl="${PREFIX}")
_common_configure_args+=(--with-tcltk-includes="-I${PREFIX}/include")
_common_configure_args+=("--with-tcltk-libs=-L${PREFIX}/lib -ltcl8.6 -ltk8.6")

#########################################################################################


export CONFIGURE_EXTRA_FLAGS='${_common_configure_args[@]} --oldincludedir=${BUILD_PREFIX}/${HOST}/sysroot/usr/include'

if [ "${PYSTON_UNOPT_BUILD}" = "1" ]; then
    make -j`nproc` unopt
    make -j`nproc` cpython_testsuite
    OUTDIR=${SRC_DIR}/build/unopt_install/usr
    PYSTON=${OUTDIR}/bin/python3
else
    RELEASE_PREFIX=${PREFIX} make -j`nproc` release
    RELEASE_PREFIX=${PREFIX} make -j`nproc` cpython_testsuite_release
    OUTDIR=${SRC_DIR}/build/release_install${PREFIX}
    PYSTON=${OUTDIR}/bin/python3.bolt
fi

cp $PYSTON ${PREFIX}/bin/python${VER}
ln -s ${PREFIX}/bin/python${VER} ${PREFIX}/bin/pyston
ln -s ${PREFIX}/bin/python${VER} ${PREFIX}/bin/pyston3

cp -r ${OUTDIR}/include/* ${PREFIX}/include/
cp -r ${OUTDIR}/lib/* ${PREFIX}/lib/

# remove pip
rm -r ${PREFIX}/lib/python${VER}/site-packages/pip*

# remove pystons site-packages directory and replace it with a symlink to cpythons default site-packages directory
# we copy in our site-package/README.txt and package it to make sure the directory get's created.
mkdir -p ${PREFIX}/lib/python${PYTHON_VERSION2}/site-packages || true
cp ${PREFIX}/lib/python${VER}/site-packages/README.txt ${PREFIX}/lib/python${PYTHON_VERSION2}/site-packages
rm -r ${PREFIX}/lib/python${VER}/site-packages
ln -s ${PREFIX}/lib/python${PYTHON_VERSION2}/site-packages/ ${PREFIX}/lib/python${VER}/site-packages

#########################################################################################
# Following code is mostly copied from the cpython recipe with some changes
# https://github.com/AnacondaRecipes/python-feedstock/blob/master-3.8/recipe/build_base.sh

declare -a _FLAGS_REPLACE=()
if [[ ${_OPTIMIZED} == yes ]]; then
  _FLAGS_REPLACE+=(-O3)
  _FLAGS_REPLACE+=(-O2)
  _FLAGS_REPLACE+=("-fprofile-use")
  _FLAGS_REPLACE+=("")
  _FLAGS_REPLACE+=("-fprofile-correction")
  _FLAGS_REPLACE+=("")
  _FLAGS_REPLACE+=("-L.")
  _FLAGS_REPLACE+=("")
  for _LTO_CFLAG in "${LTO_CFLAGS[@]}"; do
    _FLAGS_REPLACE+=(${_LTO_CFLAG})
    _FLAGS_REPLACE+=("")
  done
fi

SYSCONFIG=$(find ${OUTDIR} -name "_sysconfigdata*.py" -print0)
cat ${SYSCONFIG} | ${SYS_PYTHON} "${RECIPE_DIR}"/replace-word-pairs.py \
  "${_FLAGS_REPLACE[@]}"  \
    > ${PREFIX}/lib/python${VER}/$(basename ${SYSCONFIG})
MAKEFILE=$(find ${PREFIX}/lib/python${VER}/ -path "*config-*/Makefile" -print0)
cp ${MAKEFILE} /tmp/Makefile-$$
cat /tmp/Makefile-$$ | ${SYS_PYTHON} "${RECIPE_DIR}"/replace-word-pairs.py \
  "${_FLAGS_REPLACE[@]}"  \
    > ${MAKEFILE}
# Check to see that our differences took.
# echo diff -urN ${SYSCONFIG} ${PREFIX}/lib/python${VER}/$(basename ${SYSCONFIG})
# diff -urN ${SYSCONFIG} ${PREFIX}/lib/python${VER}/$(basename ${SYSCONFIG})

# Python installs python${VER}m and python${VER}, one as a hardlink to the other. conda-build breaks these
# by copying. Since the executable may be static it may be very large so change one to be a symlink
# of the other. In this case, python${VER}m will be the symlink.
if [[ -f ${PREFIX}/bin/python${VER}m ]]; then
  rm -f ${PREFIX}/bin/python${VER}m
  ln -s ${PREFIX}/bin/python${VER} ${PREFIX}/bin/python${VER}m
fi

# Remove test data to save space
# Though keep `support` as some things use that.
# TODO :: Make a subpackage for this once we implement multi-level testing.
pushd ${PREFIX}/lib/python${VER}
  mkdir test_keep
  mv test/__init__.py test/support test/test_support* test/test_script_helper* test_keep/
  rm -rf test */test
  mv test_keep test
popd

# Size reductions:
pushd ${PREFIX}
  if [[ -f lib/libpython${VERABI}.a ]]; then
    chmod +w lib/libpython${VERABI}.a
    ${STRIP} -S lib/libpython${VERABI}.a
  fi
  CONFIG_LIBPYTHON=$(find lib/python${VER}/config-${VERABI}* -name "libpython${VERABI}.a")
  if [[ -f lib/libpython${VERABI}.a ]] && [[ -f ${CONFIG_LIBPYTHON} ]]; then
    chmod +w ${CONFIG_LIBPYTHON}
    rm ${CONFIG_LIBPYTHON}
  fi
popd

if [[ ${HOST} =~ .*linux.* ]]; then
  mkdir -p ${PREFIX}/compiler_compat
  ln -s ${PREFIX}/bin/${HOST}-ld ${PREFIX}/compiler_compat/ld
  echo "Files in this folder are to enhance backwards compatibility of anaconda software with older compilers."   > ${PREFIX}/compiler_compat/README
  echo "See: https://github.com/conda/conda/issues/6030 for more information."                                   >> ${PREFIX}/compiler_compat/README
fi


# Copy sysconfig that gets recorded to a non-default name
#   using the new compilers with python will require setting _PYTHON_SYSCONFIGDATA_NAME
#   to the name of this file (minus the .py extension)
pushd "${PREFIX}"/lib/python${VER}
  # On Python 3.5 _sysconfigdata.py was getting copied in here and compiled for some reason.
  # This breaks our attempt to find the right one as recorded_name.
  find lib-dynload -name "_sysconfigdata*.py*" -exec rm {} \;
  recorded_name=$(find . -name "_sysconfigdata*.py")
  our_compilers_name=_sysconfigdata_$(echo ${HOST} | sed -e 's/[.-]/_/g').py
  # So we can see if anything has significantly diverged by looking in a built package.
  cp ${recorded_name} ${recorded_name}.orig
  mv ${recorded_name} ${our_compilers_name}
  PY_ARCH=$(echo ${HOST} | cut -d- -f1)

  # Copy all "${RECIPE_DIR}"/sysconfigdata/*.py. This is to support cross-compilation. They will be
  # from the previous build unfortunately so care must be taken at version bumps and flag changes.
  SRC_SYSCONFIGS=$(find "${RECIPE_DIR}"/sysconfigdata -name '*sysconfigdata*.py')
  for SRC_SYSCONFIG in ${SRC_SYSCONFIGS}; do
    DST_SYSCONFIG=$(basename ${SRC_SYSCONFIG})
    cat ${SRC_SYSCONFIG} | sed -e "s|@SGI_ABI@||g" \
                               -e "s|@ABIFLAGS@|${ABIFLAGS}|g" \
                               -e "s|@ARCH@|${PY_ARCH}|g" \
                               -e "s|@PYVERNODOTS@|${VERNODOTS}|g" \
                               -e "s|@PYVER@|${VER}|g" \
                               -e "s|@PYVERFULL@|${VERFULL}|g" \
                               -e "s|@MACOSX_DEPLOYMENT_TARGET@|${MACOSX_DEPLOYMENT_TARGET:-10.9}|g" \
                               -e "s|@TCLTK_VER@|${TCLTK_VER}|g" > ${DST_SYSCONFIG}
  done
  if [[ ${HOST} =~ .*darwin.* ]]; then
    mv _sysconfigdata_osx.py ${recorded_name}
    rm _sysconfigdata_linux.py
  else
    mv _sysconfigdata_linux.py ${recorded_name}
    rm _sysconfigdata_osx.py || true
  fi
popd

# There are some strange distutils files around. Delete them
rm -rf ${PREFIX}/lib/python${VER}/distutils/command/*.exe

$PYSTON -c "import compileall,os;compileall.compile_dir(os.environ['PREFIX'])"
rm ${PREFIX}/lib/libpython${VER}.a

#########################################################################################

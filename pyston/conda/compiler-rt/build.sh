if [[ "$target_platform" == "osx-64" ]]; then
    EXTRA_CMAKE_ARGS="-DDARWIN_osx_ARCHS=x86_64 -DCOMPILER_RT_ENABLE_IOS=Off"
    EXTRA_CMAKE_ARGS="$EXTRA_CMAKE_ARGS -DDARWIN_macosx_CACHED_SYSROOT=${CONDA_BUILD_SYSROOT} -DCMAKE_LIBTOOL=$LIBTOOL"
fi

# Prep build
cp -R "${PREFIX}/lib/cmake/llvm" "${PREFIX}/lib/cmake/modules/"

mkdir build
cd build

INSTALL_PREFIX=${PREFIX}/lib/clang/${PKG_VERSION}

cmake \
    -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE="Release" \
    -DCMAKE_PREFIX_PATH:PATH="${PREFIX}" \
    -DCMAKE_INSTALL_PREFIX:PATH="${INSTALL_PREFIX}" \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY:PATH="${INSTALL_PREFIX}/lib" \
    -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY:PATH="${INSTALL_PREFIX}/lib" \
    -DCMAKE_MODULE_PATH:PATH="${PREFIX}/lib/cmake" \
    -DLLVM_CONFIG_PATH:PATH="${PREFIX}/bin/llvm-config" \
    -DPYTHON_EXECUTABLE:PATH="${BUILD_PREFIX}/bin/python" \
    -DCMAKE_LINKER="$LD" \
    ${EXTRA_CMAKE_ARGS} \
    "${SRC_DIR}"

# Build step
make -j$CPU_COUNT VERBOSE=1

# Install step
make install -j$CPU_COUNT

# Clean up after build
rm -rf "${PREFIX}/lib/cmake/modules"

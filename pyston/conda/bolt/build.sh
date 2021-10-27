#!/bin/sh
set -eux

# we need to overwrite the conda build flags because
# using the default flags the resulting bolt binary will not work
CFLAGS="-isystem ${PREFIX}/include"
CXXFLAGS="-isystem ${PREFIX}/include"
LDFLAGS="-Wl,-rpath,${PREFIX}/lib -Wl,-rpath-link,${PREFIX}/lib -L${PREFIX}/lib"
CPPFLAGS="-isystem ${PREFIX}/include"

# without this line we can't find zlib and co..
CPPFLAGS=${CPPFLAGS}" -I${PREFIX}/include"

mkdir build
cd build
cmake -G "Unix Makefiles" $SRC_DIR/llvm -DLLVM_TARGETS_TO_BUILD="X86" -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_ASSERTIONS=ON -DLLVM_INCLUDE_TESTS=0 -DLLVM_ENABLE_PROJECTS="clang;lld;bolt"

make -j`nproc` llvm-bolt merge-fdata perf2bolt
cp bin/{llvm-bolt,merge-fdata,perf2bolt} ${PREFIX}/bin/
cp lib/libbolt_* ${PREFIX}/lib/

set -eu

cd /tmp
git clone https://github.com/pyston/pyston
cd pyston

git submodule update --init pyston/llvm pyston/bolt/bolt pyston/bolt/llvm pyston/LuaJIT pyston/macrobenchmarks
rm -rf .git
rm -rf pyston/bolt/llvm/test
find pyston/llvm -name test | xargs rm -rf

cd /tmp
tar cfz pyston_src.tar.gz pyston

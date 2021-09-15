set -eu
set -x

cd /tmp
git clone https://github.com/pyston/pyston
cd pyston

git submodule update --init pyston/llvm pyston/bolt pyston/LuaJIT pyston/macrobenchmarks
rm -rf .git
find pyston/llvm pyston/bolt -name test | grep -v bolt/test | xargs rm -rf

cd /tmp
tar cfz pyston_src.tar.gz pyston

mkdir build
if errorlevel 1 exit 1

cd build
if errorlevel 1 exit 1

set BUILD_CONFIG=Release
if errorlevel 1 exit 1

set "CC=clang-cl.exe"
set "CXX=clang-cl.exe"

set "INSTALL_PREFIX=%LIBRARY_PREFIX%\lib\clang\%PKG_VERSION%"

cmake ^
    -G "NMake Makefiles" ^
    -DCMAKE_BUILD_TYPE="Release" ^
    -DCMAKE_PREFIX_PATH:PATH="%LIBRARY_PREFIX%" ^
    -DCMAKE_INSTALL_PREFIX:PATH="%INSTALL_PREFIX%" ^
    -DCMAKE_MODULE_PATH:PATH="%LIBRARY_LIB%\cmake" ^
    -DLLVM_CONFIG_PATH:PATH="%LIBRARY_BIN%\llvm-config.exe" ^
    "%SRC_DIR%"
if errorlevel 1 exit 1

:: Build step
nmake
if errorlevel 1 exit 1

:: Install step
nmake install
if errorlevel 1 exit 1

mkdir %PREFIX%\lib\clang\%PKG_VERSION%\lib\windows
copy %INSTALL_PREFIX%\lib\windows\* %PREFIX%\lib\clang\%PKG_VERSION%\lib\windows\

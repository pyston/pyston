# Pyston

Pyston is a fork of CPython 3.8.8 with additional optimizations for performance.  It is targeted at large real-world applications such as web serving, delivering up to a 30% speedup with no development work required.

[Blog](https://blog.pyston.org/)

[Website](https://pyston.org/)

[Mailing list](http://eepurl.com/hops6n)

[Discord](https://discord.gg/S7gsqnb)

## Techniques

We plan on explaining our techniques in more detail in future blog posts, but the main ones we use are:

- A very-low-overhead JIT using DynASM
- Quickening
- Aggressive attribute caching
- General CPython optimizations
- Build process improvements

## Dependencies

First do

```
git submodule update --init pyston/llvm pyston/bolt/bolt pyston/LuaJIT pyston/macrobenchmarks
```

Pyston has the following build dependencies:

```
sudo apt-get install build-essential ninja-build git cmake clang libssl-dev libsqlite3-dev luajit python3.8 zlib1g-dev virtualenv libjpeg-dev linux-tools-common linux-tools-generic linux-tools-`uname -r`
```

Extra dependencies for running the test suite:
```
sudo apt-get install libwebp-dev libjpeg-dev python3.8-gdbm python3.8-tk python3.8-dev tk-dev libgdbm-dev libgdbm-compat-dev liblzma-dev libbz2-dev nginx
```

Extra dependencies for producing Pyston debian packages and portable directory release:
```
sudo apt-get install dh-make dh-exec debhelper patchelf
```

## Building

For a build with all optimizations enabled (LTO+PGO) run:

```
make -j
```

An initial build will take quite a long time due to having to build LLVM twice, and subsequent builds are faster but still slow due to extra profiling steps.

A symlink to the final binary will be created with the name `pyston3`

For a quicker build during development run:
```
make unopt -j
```
the generated executable can be found inside `pyston/build/cpython_unopt_install/`

Running a python file called script.py with pyston can be easily done via:
```
make script_unopt
```
or
```
make script_opt
```

## Changing the version number
1. adjust `VERSION` in `pyston/tools/make_release.sh` and `pyston/tools/bench_release.sh`
2. add a `pyston/debian/changelog` entry (make sure the date is correct or the build fails in odd ways)
3. adjust `PYSTON_*_VERSION` inside `Include/patchlevel.h`
4. adjust `PYSTON_VERSION` inside `configure.ac` and run `autoconf`
5. update `PYSTON_MINOR` and similar inside Makefile
6. update the include directory in pyston/pystol/CMakeLists.txt

## Release packaging
We use a script which builds automatically packages for all supported distributions via docker (will take several hours):
NOTE: our release build process requires hardware with LBR (last branch record) support (=non virtualized Intel CPU).
1. make sure your repos do not contain unwanted changes because they will get used to build the packages
2. execute `$ pyston/tools/make_release.sh`
3. output debian packages are now in `release/<version>/`.
   Inside this directory are also different "portable dir" releases made from the different distibution deb packages.
   Decide on which portable dir to use - the oldest version will run with most dists but will also bundle the oldes libs.
4. execute `$ make tune; pyston/tools/bench_release.sh` to generate benchmark results.

## Checking for Pyston at runtime

Our current recommended way to see if your Python code is running on Pyston is to do `hasattr(sys, "pyston_version_info")`.

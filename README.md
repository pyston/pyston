# Pyston

Pyston is a fork of CPython 3.8.12 with additional optimizations for performance.  It is targeted at large real-world applications such as web serving, delivering up to a 30% speedup with no development work required.

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

## Docker images

We have some experimental docker images on DockerHub with Pyston pre-installed, you can quickly try out Pyston by doing

```
docker run -it pyston/pyston
```

You could also attempt to use this as your base image, and `python` will be provided by Pyston.

The default image contains quite a few libraries for compiling extension modules, and if you'd like a smaller image we also have a `pyston/slim` version that you can use.

These have not been heavily tested, so if you run into any issues please report them to our tracker.

## Checking for Pyston at runtime

Our current recommended way to see if your Python code is running on Pyston is to do `hasattr(sys, "pyston_version_info")`.

## Installing packages

Pyston is *API compatible* but not *ABI compatible* with CPython. This means that C extensions will work, but they need to be recompiled.

Typically with Python one will download and install pre-compiled packages, but with Pyston there are currently not pre-compiled packages available (we're working on that) so the compilation step will run when you install them. This means that you may run into problems installing packages on Pyston when they work on CPython: the same issues you would have trying to recompile these packages for CPython.

Many packages have build-time dependencies that you will need to install to get them to work. For example to `pip install cryptography` you need a Rust compiler, such as by doing `sudo apt-get install rustc`.

## History

Pyston was started at Dropbox in 2014 in order to reduce the costs of its rapidly-growing Python server fleet. That version of Pyston is now called "Pyston v1", and is located [here](https://github.com/pyston/pyston_v1). Pyston v1 was a from-scratch implementation of Python 2.7 that featured a conservative tracing garbage collector and a LLVM-based compilation tier. The tracing garbage collector was eventually replaced with reference counting, and a faster-to-compile baseline JIT was added as well.

At the same time that Pyston was being developed, Dropbox was in-parallel investigating other languages as the primary development language for the company. In 2017 it was decided that this was the preferrable approach, and the Pyston project was shut down. At this time Pyston v1 was able to run the Dropbox codebase, but with several caveats such as increased memory and numerous small compatibility challenges.

In 2019 the Pyston developers regrouped without a corporate sponsor and started investigating alternative approaches to speeding up Python. They ended up deciding to fork CPython 3.8, and by early 2020 they restarted the project in a new codebase, and called it "Pyston v2". The first version of Pyston v2 was released in late 2020.

In mid-2021 the Pyston developers joined Anaconda, which since then has provided funding for the project and packaging expertise.

# Building Pyston

## Build dependencies

First do

```
git submodule update --init pyston/llvm pyston/bolt pyston/LuaJIT pyston/macrobenchmarks
```

Pyston has the following build dependencies:

```
sudo apt-get install build-essential git cmake clang libssl-dev libsqlite3-dev luajit python3.8 zlib1g-dev virtualenv libjpeg-dev linux-tools-common linux-tools-generic linux-tools-`uname -r`
```

Extra dependencies for running the test suite:
```
sudo apt-get install libwebp-dev libjpeg-dev python3.8-gdbm python3.8-tk python3.8-dev tk-dev libgdbm-dev libgdbm-compat-dev liblzma-dev libbz2-dev nginx rustc time libffi-dev
```

Extra dependencies for producing Pyston debian packages and portable directory release:
```
sudo apt-get install dh-make dh-exec debhelper patchelf
```

Extra dependencies for producing Pyston docker images (on amd64 adjust for arm64):
```
# docker buildx
wget https://github.com/docker/buildx/releases/download/v0.8.1/buildx-v0.8.1.linux-amd64 -O $HOME/.docker/cli-plugins/docker-buildx
chmod +x $HOME/.docker/cli-plugins/docker-buildx
# qemu
docker run --privileged --rm tonistiigi/binfmt --install arm64
```

## Building

For a build with all optimizations enabled (LTO+PGO) run:

```
make -j`nproc`
```

An initial build will take quite a long time due to having to build LLVM twice, and subsequent builds are faster but still slow due to extra profiling steps.

A symlink to the final binary will be created with the name `pyston3`

For a quicker build during development run:
```
make unopt -j`nproc`
```
the generated executable can be found inside `build/unopt_install/`

Running a python file called script.py with pyston can be easily done via:
```
make script_unopt
```
or
```
make script_opt
```

### Building pyston-lite

```
make -j`nproc` bolt
cd pyston/pyston_lite
python3 setup.py build
```

You can set the `NOBOLT=1` environment variable for setup.py if you'd like to skip building bolt. You can also pass `NOPGO=1` and `NOLTO=1` if you'd like the fastest build times, such as for development.

If you like to build pyston-lite with BOLT (currently only used on x86 Linux) for all supported CPython versions you will need to have python3.7 to python3.10 installed and in your path.
For Ubuntu this can most easily done by adding the deadsnakes PPA:

```
sudo add-apt-repository ppa:deadsnakes/ppa
sudo apt update
sudo apt-get install python3.7-full python3.8-full python3.9-full python3.10-full
```

To compile wheels for all supported CPython versions and output them into wheelhouse/ run:
```
make package
```

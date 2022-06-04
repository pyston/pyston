## Changing the version number
1. adjust `VERSION` in `pyston/tools/make_release.sh` and `pyston/tools/bench_release.sh`
2. add a `pyston/debian/changelog` entry (make sure the date is correct or the build fails in odd ways)
3. adjust `PYSTON_*_VERSION` inside `Include/patchlevel.h`
4. update `pyston/docker/Dockerfile\*` and `pyston/docker/build_docker.sh`
5. update `pyston/conda/{pyston,python,python_abi,pyston_lite}/meta.yaml` 
   and update `build_num` if necessary (mandatory for `python` and `python_abi`)
6. update "version" `pyston/conda/installer/construct.yaml`
7. update `pyston/pyston_lite/setup.py` and `pyston/pyston_lite/autoload/setup.py`

In addition if more than only the micro version changes:
1. adjust `PYSTON_VERSION` inside `configure.ac` and run `autoconf`
2. update `PYSTON_MINOR` and similar inside Makefile
3. update the include directory in `pyston/pystol/CMakeLists.txt`
4. update `pyston/debian/pyston.{install,links,postinst,prerm}`

## Release packaging
We use a script which builds automatically packages for all supported distributions via docker (will take several hours) but needs to be run for Amd64 and Arm64:
1. make sure your repos do not contain unwanted changes because they will get used to build the packages
2. execute `$ pyston/tools/make_release.sh`
3. output debian packages are now in `release/<version>/`.
   Inside this directory are also different "portable dir" releases made from the different distibution deb packages.
   Decide on which portable dir to use - the oldest version will run with most dists but will also bundle the oldes libs.
4. execute `$ make tune; pyston/tools/bench_release.sh` to generate benchmark results.
5. Generate new conda packages using `pyston/conda/build_pkgs.sh` and upload them.
6. Generate new conda installer using `pyston/conda/build_installer.sh`
7. Upload to github
8. Generate new docker images using `pyston/docker/build_docker.sh` (Generates Arm and X86 at same time)
9. Update https://github.com/pyston/pyston/wiki/Using-conda

## Releasing pyston-lite
```bash
cd pyston/pyston_lite
git clean -fxdi
make package
make upload_wheels
make test_packages
```

## Testing a release
1. Build the release as above
2. Install the .deb package
3. `pyston -m test -j 0`
4. (Optional) Run the integration tests
5. Test the performance of the release
- Multiple options, but one is to create a `build/systempyston_env` and use `pyston/tools/benchmarks_runners/Makefile`

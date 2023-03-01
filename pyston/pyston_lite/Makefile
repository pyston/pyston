.DEFAULT_GOAL := env/lite.stamp

PYTHON?=python3.8
POLICY?=manylinux2014

package: build_wheels

ARCH:=$(shell uname -m)
ifeq ($(ARCH),x86_64)
	BOLT_DOCKERFLAGS:=-e BOLTFLAGS=1
	BOLT_CMD:=python3 bolt_wheel.py wheelhouse/*.whl
endif

build_wheels:
	git clean -x -i -d -f .
	bash -c 'if [ -d wheelhouse ]; then sudo rm -rf wheelhouse ; fi'
	$(MAKE) build_wheels_noclean

ifeq ($(shell uname), Darwin)
build_wheels_noclean:
	./build_wheels_macos.sh
else
build_wheels_noclean:
	docker run -e PYSTON_LITE_NAME -e NOBOLT=1 $(BOLT_DOCKERFLAGS) -e PLAT=$(POLICY)_$(ARCH) -e POLICY \
		-v `pwd`/../../:/io quay.io/pypa/$(POLICY)_$(ARCH) \
		bash -c "bash /io/pyston/pyston_lite/build_wheels.sh && chown -R $(shell id -u):$(shell id -g) /io/pyston/pyston_lite/"
	$(BOLT_CMD)
endif

upload_wheels: env
	env/bin/pip install twine
	./env/bin/twine upload wheelhouse/*.whl || true

test_packages:
	rm -rf /tmp/env
	$(PYTHON) -m venv /tmp/env
	/tmp/env/bin/pip install wheel
	/tmp/env/bin/pip install --upgrade pyston_lite_autoload
	time /tmp/env/bin/python -c 'for i in range(100000000): pass'
	DISABLE_PYSTON=1 time /tmp/env/bin/python -c 'for i in range(100000000): pass'


######
# The rest of this file is for testing things:

env:
	$(PYTHON) -m venv env
	touch $@

env/lite.stamp: $(wildcard *.c) $(wildcard *.h) | env
	./env/bin/python setup.py install
	cd autoload; ../env/bin/python setup.py install
	touch $@

env/update.stamp: env/lite.stamp
	./env/bin/pip install -r ../pgo_requirements.txt
	touch $@


# Skip the tests in ../test/ that test within virtual environments, because pyston-lite
# won't get installed into them and we won't end up testing anything.
#
# test_code has a bug when run with another client that calls RequestCodeExtraIndex
# note: the testsuite doesn't pass for my Ubuntu system cpython, but it does pass
# if I reconfigure this makefile to use a manually-built cpython
#
# A number of other tests in the cpython testsuite fail with the Ubuntu packages, and I'm not sure why.
# This happens regardless of whether pyston-lite is installed, so just exclude them.
ADDITIONAL_TESTS_TO_SKIP?=
ifeq ($(shell uname),Darwin)
# this test errors on macOS (also with stock cpython)
ADDITIONAL_TESTS_TO_SKIP:=$(ADDITIONAL_TESTS_TO_SKIP) test_ctypes
endif
ifeq ($(shell uname -m),arm64) # only triggers on macOS (linux returns aarch64)
# this tests crash on arm64 macOS (also with stock cpython)
ADDITIONAL_TESTS_TO_SKIP:=$(ADDITIONAL_TESTS_TO_SKIP) test_dbm test_dbm_ndbm
endif
test: env/lite.stamp
	set -ex; for fn in ../test/*.py; do if [ $$fn = ../test/test_rebuild_packages.py -o $$fn = ../test/test_venvs.py ]; then continue; fi; ./env/bin/python $$fn; done
	./env/bin/python -c 'import test.support; test.support.check_impl_detail = lambda **kw: False; import test.test_code; test.test_code.test_main()'
	./env/bin/python -m test -j0 -x test_code test_distutils test_ensurepip test_minidom test_site test_xml_etree test_xml_etree_c test_capi $(ADDITIONAL_TESTS_TO_SKIP)



VIRTUALENV:=../../build/bootstrap_env/bin/virtualenv
$(VIRTUALENV):
	virtualenv -p $(PYTHON) ../../build/bootstrap_env
	../../build/bootstrap_env/bin/pip install virtualenv

# args: test_suffix test_binary
define make_test_build
$(eval
../test/external/test_%.$(1): ../test/external/test_%.py $(2) | $(VIRTUALENV)
	OPENSSL_CONF=$(abspath ../test/openssl_dev.cnf) bash -c "set -o pipefail; $(2) -u $$< 2>&1 | tee $$@_"
	mv $$@_ $$@
)
endef

$(call make_test_build,liteexpected,$(shell which $(PYTHON)))
.PRECIOUS: ../test/external/test_%.liteexpected ../test/external/test_%.liteoutput
$(call make_test_build,liteoutput,env/bin/python)

.PHONY: test_%
test_%: ../test/external/test_%.liteexpected ../test/external/test_%.liteoutput
	$(PYTHON) ../test/external/helpers.py compare $< $(patsubst %.liteexpected,%.liteoutput,$<)

EXTERNAL_TESTSUITES:=django urllib3 setuptools six requests sqlalchemy pandas numpy
testsuites: env/lite.stamp $(patsubst %,test_%,$(EXTERNAL_TESTSUITES))


ENV:=
BENCH:=../macrobenchmarks/benchmarks/bm_flaskblogging/run_benchmark.py --legacy


bench: env/update.stamp
	$(ENV) ./env/bin/python $(BENCH)

bench_baseline: env/update.stamp
	DISABLE_PYSTON=1 ./env/bin/python $(BENCH)

perf_bench: env/update.stamp
	$(ENV) JIT_PERF_MAP=1 perf record -o perf_lite.data ./env/bin/python $(BENCH)
	$(MAKE) perf_report
perf_report:
	perf report -i perf_lite.data -n --objdump=../../pyston/tools/perf_jit.py

perf_bench_baseline: env/update.stamp
	DISABLE_PYSTON=1 JIT_PERF_MAP=1 perf record -o perf_baseline.data ./env/bin/python $(BENCH)
	$(MAKE) perf_report_baseline
perf_report_baseline:
	perf report -i perf_baseline.data -n --objdump=../../pyston/tools/perf_jit.py

dbg_bench: env/update.stamp
	DISABLE_PYSTONFULL=1 $(ENV) gdb --args ./env/bin/python $(BENCH) 1000000


bench_full:
	$(MAKE) -C ../.. build/unoptshared_env/update.stamp
	$(ENV) LD_LIBRARY_PATH=../../build/unoptshared_install/usr/lib:$$LD_LIBRARY_PATH ../../build/unoptshared_env/bin/python $(BENCH)

perf_bench_full:
	$(MAKE) -C ../.. build/unoptshared_env/update.stamp
	$(ENV) LD_LIBRARY_PATH=../../build/unoptshared_install/usr/lib:$$LD_LIBRARY_PATH JIT_PERF_MAP=1 perf record -o perf_full.data ../../build/unoptshared_env/bin/python $(BENCH)
	$(MAKE) perf_report_full
perf_report_full:
	perf report -i perf_full.data -n --objdump=../../pyston/tools/perf_jit.py

dbg_bench_full:
	$(MAKE) -C ../.. build/unoptshared_env/update.stamp
	$(ENV) LD_LIBRARY_PATH=../../build/unoptshared_install/usr/lib:$$LD_LIBRARY_PATH gdb --args ../../build/unoptshared_env/bin/python $(BENCH) 1000000


bench_fullopt:
	$(MAKE) -C ../.. build/optshared_env/update.stamp
	$(ENV) LD_LIBRARY_PATH=../../build/optshared_install/usr/lib:$$LD_LIBRARY_PATH ../../build/optshared_env/bin/python $(BENCH)

perf_bench_fullopt:
	$(MAKE) -C ../.. build/optshared_env/update.stamp
	$(ENV) LD_LIBRARY_PATH=../../build/optshared_install/usr/lib:$$LD_LIBRARY_PATH JIT_PERF_MAP=1 perf record ../../build/optshared_env/bin/python $(BENCH)
	perf report -n

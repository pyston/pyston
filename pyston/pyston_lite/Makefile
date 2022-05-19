.DEFAULT_GOAL := pyston_lite.so

# Change these to target a different Python:
PYTHON:=python3.8
DIR:=python3.8
PYTHON_INCL:=/usr/include/$(DIR)/


aot_ceval_jit.prep.c: aot_ceval_jit.c
	../../pyston/tools/dynasm_preprocess.py $< $@

aot_ceval_jit.gen.c: aot_ceval_jit.prep.c
	luajit ../../pyston/LuaJIT/dynasm/dynasm.lua  -o aot_ceval_jit.gen.c $<

CC:=gcc
CFLAGS:=$(CFLAGS) $(PY_CORE_CFLAGS) -I../../pyston/LuaJIT/ -Wno-address -Wpointer-to-int-cast -I$(PYTHON_INCL) -I$(PYTHON_INCL)/internal -DPYSTON_SPEEDUPS -DPy_BUILD_CORE -fPIC -Werror=implicit-function-declaration -g
CFLAGS:=$(CFLAGS) -pthread -c -Wno-unused-result -Wsign-compare -DNDEBUG -g -fwrapv -Wall -DENABLE_AOT   -fstack-protector -specs=../tools/no-pie-compile.specs -D_FORTIFY_SOURCE=2 -fno-reorder-blocks-and-partition -std=c99 -Wextra -Wno-unused-result -Wno-unused-parameter -Wno-missing-field-initializers -Werror=implicit-function-declaration  -I../../Include/internal -IObjects -IInclude -IPython -I. -I../../Include   -I../aot  -DPy_BUILD_CORE -Wno-unused-label
CFLAGS:=$(CFLAGS) -std=gnu99 -fcf-protection=none
CFLAGS:=$(CFLAGS) -O3
CFLAGS:=$(CFLAGS) -DPYSTON_LITE

pyston_lite.so: aot_ceval_jit.gen.o aot_ceval_jit_helper.o lib.o aot_ceval.o
	$(CC) -shared $^ -o $@

%.o: %.c $(wildcard *.h)

.PHONY: clean
clean:
	rm -rf *.o *.so env


package: package_jit package_autoload

package_jit:
	git clean -x -i -d -f .
	bash -c 'if [ -d wheelhouse ]; then sudo rm -rf wheelhouse ; fi'
	docker run --rm -e PLAT=manylinux2014_x86_64 -v `pwd`/../../:/io quay.io/pypa/manylinux2014_x86_64 bash -c "bash /io/pyston/pyston_lite/build_wheels.sh && chown -R $(shell id -u):$(shell id -g) /io/pyston/pyston_lite/wheelhouse"

package_autoload: env
	bash -c "cd autoload; rm -rf dist; ../env/bin/python setup.py sdist"

upload_wheels: env
	env/bin/pip install twine
	./env/bin/twine upload autoload/dist/pyston_lite_autoload* || true
	./env/bin/twine upload wheelhouse/*manylinux*.whl || true

test_packages:
	rm -rf /tmp/env
	python3.8 -m venv /tmp/env
	/tmp/env/bin/pip install --upgrade pyston_lite_autoload
	time /tmp/env/bin/python -c 'for i in range(100000000): pass'
	DISABLE_PYSTON=1 time /tmp/env/bin/python -c 'for i in range(100000000): pass'


######
# The rest of this file is for testing things:

env: pyston_lite.so
	$(PYTHON) -m venv env
	cp pyston_lite.so autoload/pyston_lite_autoload.pth env/lib/$(DIR)/site-packages/

env/update.stamp: env
	./env/bin/pip install -r ../pgo_requirements.txt
	touch $@


ENV:=
BENCH:=../macrobenchmarks/benchmarks/bm_flaskblogging/run_benchmark.py --legacy


# test_code has a bug when run with another client that calls RequestCodeExtraIndex
# note: the testsuite doesn't pass for my Ubuntu system cpython, but it does pass
# if I reconfigure this makefile to use a manually-built cpython
testsuite: env
	./env/bin/python -c 'import test.support; test.support.check_impl_detail = lambda **kw: False; import test.test_code; test.test_code.test_main()'
	./env/bin/python -m test -j0 -x test_code

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

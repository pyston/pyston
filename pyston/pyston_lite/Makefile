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




######
# The rest of this file is for testing things:

env: pyston_lite.so
	$(PYTHON) -m venv env
	cp pyston_lite.so pyston_lite_autoload.pth env/lib/$(DIR)/site-packages/

env/update.stamp: env
	./env/bin/pip install -r ../pgo_requirements.txt
	touch $@

test: env
	./env/bin/python ../test/pyston_lite_midexecution.py


ENV:=PYSTON=1 SHOW_OPCACHE_STATS=1 JIT_PERF_MAP=1
BENCH:=../macrobenchmarks/benchmarks/bm_flaskblogging/run_benchmark.py --legacy


# test_code has a bug when run with another client that calls RequestCodeExtraIndex
# note: the testsuite doesn't pass for my Ubuntu system cpython, but it does pass
# if I reconfigure this makefile to use a manually-built cpython
testsuite: env
	PYSTON=1 ./env/bin/python -c 'import test.support; test.support.check_impl_detail = lambda **kw: False; import test.test_code; test.test_code.test_main()'
	PYSTON=1 ./env/bin/python -m test -j0 -x test_code

bench: env/update.stamp
	$(ENV) ./env/bin/python $(BENCH)

bench_baseline: env/update.stamp
	$(patsubst PYSTON=1,,$(ENV)) ./env/bin/python $(BENCH)

perf_bench: env/update.stamp
	DISABLE_PYSTONFULL=1 $(ENV) perf record -o perf_lite.data ./env/bin/python $(BENCH)
	$(MAKE) perf_report
perf_report:
	perf report -i perf_lite.data -n

perf_bench_baseline: env/update.stamp
	DISABLE_PYSTONFULL=1 $(patsubst PYSTON=1,,$(ENV)) perf record -o perf_baseline.data ./env/bin/python $(BENCH)
	$(MAKE) perf_report_baseline
perf_report_baseline:
	perf report -i perf_baseline.data -n

dbg_bench: env/update.stamp
	DISABLE_PYSTONFULL=1 $(ENV) gdb --args ./env/bin/python $(BENCH) 1000000


bench_full:
	$(MAKE) -C ../.. build/unoptshared_env/update.stamp
	$(ENV) LD_LIBRARY_PATH=../../build/unoptshared_install/usr/lib:$$LD_LIBRARY_PATH ../../build/unoptshared_env/bin/python $(BENCH)

perf_bench_full:
	$(MAKE) -C ../.. build/unoptshared_env/update.stamp
	$(ENV) LD_LIBRARY_PATH=../../build/unoptshared_install/usr/lib:$$LD_LIBRARY_PATH perf record -o perf_full.data ../../build/unoptshared_env/bin/python $(BENCH)
	$(MAKE) perf_report_full
perf_report_full:
	perf report -i perf_full.data -n

dbg_bench_full:
	$(MAKE) -C ../.. build/unoptshared_env/update.stamp
	$(ENV) LD_LIBRARY_PATH=../../build/unoptshared_install/usr/lib:$$LD_LIBRARY_PATH gdb --args ../../build/unoptshared_env/bin/python $(BENCH) 1000000


bench_fullopt:
	$(MAKE) -C ../.. build/optshared_env/update.stamp
	$(ENV) LD_LIBRARY_PATH=../../build/optshared_install/usr/lib:$$LD_LIBRARY_PATH ../../build/optshared_env/bin/python $(BENCH)

perf_bench_fullopt:
	$(MAKE) -C ../.. build/optshared_env/update.stamp
	$(ENV) LD_LIBRARY_PATH=../../build/optshared_install/usr/lib:$$LD_LIBRARY_PATH perf record ../../build/optshared_env/bin/python $(BENCH)
	perf report -n

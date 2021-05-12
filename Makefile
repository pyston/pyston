.SUFFIXES:

CLANG:=pyston/build/Release/llvm/bin/clang
GDB:=gdb
.DEFAULT_GOAL:=all

-include Makefile.local

.PHONY: all
all: pyston3

pyston3: pyston/build/opt_env/bin/python
	ln -sf $< $@

unopt: pyston/build/unopt_env/bin/python

.PHONY: clean
clean:
	rm -rf pyston/build/*_env pyston/build/cpython_* pyston/aot/*.o pyston/aot/*.so pyston/aot/*.bc pyston/aot/*.c pyston/aot/*.gcda pyston/aot/aot_prof pyston/aot/*.profdata

tune: pyston/build/system_env/bin/python
	PYTHONPATH=pyston/tools pyston/build/system_env/bin/python -c "import tune; tune.tune()"
tune_reset: pyston/build/system_env/bin/python
	PYTHONPATH=pyston/tools pyston/build/system_env/bin/python -c "import tune; tune.untune()"

pyston/build/Release/build.ninja:
	mkdir -p pyston/build/Release
	@# Use gold linker since ld 2.32 (Ubuntu 19.04) is unable to link compiler-rt:
	cd pyston/build/Release; CC=clang CXX=clang++ cmake ../.. -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_USE_LINKER=gold -DLLVM_ENABLE_PROJECTS="clang;compiler-rt" -DLLVM_USE_PERF=ON

pyston/build/PartialDebug/build.ninja:
	mkdir -p pyston/build/PartialDebug
	cd pyston/build/PartialDebug; CC=clang CXX=clang++ cmake ../.. -G Ninja -DCMAKE_BUILD_TYPE=PartialDebug -DLLVM_ENABLE_PROJECTS=clang -DLLVM_USE_PERF=ON

pyston/build/Debug/build.ninja:
	mkdir -p pyston/build/Debug
	cd pyston/build/Debug; CC=clang CXX=clang++ cmake ../.. -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLLVM_ENABLE_PROJECTS=clang -DLLVM_USE_PERF=ON

.PHONY: build_release build_dbg build_debug
build_dbg: pyston/build/PartialDebug/build.ninja pyston/build/bc_env/bin/python
	cd pyston/build/PartialDebug; ninja libinterp.so libpystol.so
build_debug: pyston/build/Debug/build.ninja pyston/build/bc_env/bin/python
	cd pyston/build/Debug; ninja libinterp.so libpystol.so

# The "%"s here are to force make to consider these as group targets and not run this target multiple times
pyston/build/Release/nitrous/libinterp%so pyston/build/Release/pystol/libpystol%so: pyston/build/Release/build.ninja pyston/build/bc_env/bin/python $(wildcard pyston/nitrous/*.cpp) $(wildcard pyston/nitrous/*.h) $(wildcard pyston/pystol/*.cpp) $(wildcard pyston/pystol/*.h)
	cd pyston/build/Release; ninja libinterp.so libpystol.so
	@# touch them since our dependencies in this makefile are not 100% correct, and they might not have gotten rebuilt:
	touch pyston/build/Release/nitrous/libinterp.so pyston/build/Release/pystol/libpystol.so
build_release:
	$(MAKE) pyston/build/Release/nitrous/libinterp.so pyston/build/Release/pystol/libpystol.so

LLVM_TOOLS:=$(CLANG)
.PHONY: clang
clang $(CLANG): | pyston/build/Release/build.ninja
	cd pyston/build/Release; ninja clang llvm-dis llvm-as llvm-link opt compiler-rt llvm-profdata

pyston/bolt/llvm/tools/llvm-bolt:
	ln -fs $(shell dirname $(realpath $(firstword $(MAKEFILE_LIST))))/pyston/bolt/bolt $@
	cd pyston/bolt/llvm; patch -p 1 < tools/llvm-bolt/llvm.patch

BOLT:=pyston/build/bolt/bin/llvm-bolt
MERGE_FDATA:=pyston/build/bolt/bin/merge-fdata
pyston/build/bolt/build.ninja: pyston/bolt/llvm/tools/llvm-bolt
	mkdir -p pyston/build/bolt
	cd pyston/build/bolt; cmake -G Ninja ../../bolt/llvm -DLLVM_TARGETS_TO_BUILD="X86" -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_ASSERTIONS=ON
bolt: $(BOLT)
$(BOLT): pyston/build/bolt/build.ninja
	cd pyston/build/bolt; ninja llvm-bolt merge-fdata

# this flags are what the default debian/ubuntu cpython uses
CPYTHON_EXTRA_CFLAGS:=-fstack-protector -specs=$(CURDIR)/pyston/tools/no-pie-compile.specs -D_FORTIFY_SOURCE=2 -fPIE
CPYTHON_EXTRA_LDFLAGS:=-specs=$(CURDIR)/pyston/tools/no-pie-link.specs -Wl,-z,relro -fPIE

PROFILE_TASK:=../../../Lib/test/regrtest.py -j 1 -unone,decimal -x test_posix test_asyncio test_cmd_line_script test_compiler test_concurrent_futures test_ctypes test_dbm_dumb test_dbm_ndbm test_distutils test_ensurepip test_ftplib test_gdb test_httplib test_imaplib test_ioctl test_linuxaudiodev test_multiprocessing test_nntplib test_ossaudiodev test_poplib test_pydoc test_signal test_socket test_socketserver test_ssl test_subprocess test_sundry test_thread test_threaded_import test_threadedtempfile test_threading test_threading_local test_threadsignals test_venv test_zipimport_support

MAKEFILE_DEPENDENCIES:=Makefile.pre.in configure
pyston/build/cpython_bc_build/Makefile: $(CLANG) $(MAKEFILE_DEPENDENCIES)
	mkdir -p pyston/build/cpython_bc_build
	cd pyston/build/cpython_bc_build; WRAPPER_REALCC=$(realpath $(CLANG)) WRAPPER_OUTPUT_PREFIX=../cpython_bc CC=../../../pyston/clang_wrapper.py CFLAGS_NODIST="$(CPYTHON_EXTRA_CFLAGS) -Wno-unused-command-line-argument" LDFLAGS_NODIST="$(CPYTHON_EXTRA_LDFLAGS)" ../../../configure --prefix=/usr --disable-aot --disable-debugging-features --enable-configure
pyston/build/cpython_unopt_build/Makefile: $(MAKEFILE_DEPENDENCIES)
	mkdir -p pyston/build/cpython_unopt_build
	cd pyston/build/cpython_unopt_build; CC=gcc CFLAGS_NODIST="$(CPYTHON_EXTRA_CFLAGS) -fno-reorder-blocks-and-partition" LDFLAGS_NODIST="$(CPYTHON_EXTRA_LDFLAGS) -Wl,--emit-relocs" ../../../configure --prefix=/usr --disable-debugging-features --enable-configure
pyston/build/cpython_opt_build/Makefile: $(MAKEFILE_DEPENDENCIES)
	mkdir -p pyston/build/cpython_opt_build
	cd pyston/build/cpython_opt_build; PROFILE_TASK="$(PROFILE_TASK)" CC=gcc CFLAGS_NODIST="$(CPYTHON_EXTRA_CFLAGS) -fno-reorder-blocks-and-partition" LDFLAGS_NODIST="$(CPYTHON_EXTRA_LDFLAGS) -Wl,--emit-relocs" ../../../configure --prefix=/usr --enable-optimizations --with-lto --disable-debugging-features --enable-configure
# We have to --disable-debugging-features for consistency with the bc build
# If we had a separate bc-dbg build then we could change this
pyston/build/cpython_dbg_build/Makefile: $(MAKEFILE_DEPENDENCIES)
	mkdir -p pyston/build/cpython_dbg_build
	cd pyston/build/cpython_dbg_build; CC=gcc CFLAGS_NODIST="$(CPYTHON_EXTRA_CFLAGS) -fno-reorder-blocks-and-partition" LDFLAGS_NODIST="$(CPYTHON_EXTRA_LDFLAGS) -Wl,--emit-relocs" ../../../configure --prefix=/usr --with-pydebug --disable-debugging-features --enable-configure
pyston/build/cpython_stock_build/Makefile: $(MAKEFILE_DEPENDENCIES)
	mkdir -p pyston/build/cpython_stock_build
	cd pyston/build/cpython_stock_build; PROFILE_TASK="$(PROFILE_TASK) || true" CC=gcc CFLAGS_NODIST="$(CPYTHON_EXTRA_CFLAGS)" LDFLAGS_NODIST="$(CPYTHON_EXTRA_LDFLAGS)" ../../../configure --prefix=/usr --enable-optimizations --with-lto --disable-pyston --enable-configure
pyston/build/cpython_stockunopt_build/Makefile: $(MAKEFILE_DEPENDENCIES)
	mkdir -p pyston/build/cpython_stockunopt_build
	cd pyston/build/cpython_stockunopt_build; CC=gcc CFLAGS_NODIST="$(CPYTHON_EXTRA_CFLAGS)" LDFLAGS_NODIST="$(CPYTHON_EXTRA_LDFLAGS)" ../../../configure --prefix=/usr --disable-pyston --enable-configure
pyston/build/cpython_stockdbg_build/Makefile: $(MAKEFILE_DEPENDENCIES)
	mkdir -p pyston/build/cpython_stockdbg_build
	cd pyston/build/cpython_stockdbg_build; CC=gcc CFLAGS_NODIST="$(CPYTHON_EXTRA_CFLAGS)" LDFLAGS_NODIST="$(CPYTHON_EXTRA_LDFLAGS)" ../../../configure --prefix=/usr --disable-pyston --with-pydebug --enable-configure

pyston/build/cpython_bc_build/pyston: pyston/build/cpython_bc_build/Makefile $(filter-out $(wildcard Python/aot*.c),$(wildcard */*.c)) $(wildcard */*.h)
	cd pyston/build/cpython_bc_build; WRAPPER_REALCC=$(realpath $(CLANG)) WRAPPER_OUTPUT_PREFIX=../cpython_bc $(MAKE)
	touch $@ # some cpython .c files don't affect the python executable
pyston/build/cpython_unopt_build/pyston: pyston/build/cpython_unopt_build/Makefile $(wildcard */*.c) $(wildcard */*.h) pyston/aot/aot_all.bc
	cd pyston/build/cpython_unopt_build; $(MAKE)
	touch $@ # some cpython .c files don't affect the python executable
pyston/build/cpython_opt_build/pyston: pyston/build/cpython_opt_build/Makefile $(wildcard */*.c) $(wildcard */*.h) pyston/aot/aot_all.bc
	cd pyston/build/cpython_opt_build; $(MAKE)
	touch $@ # some cpython .c files don't affect the python executable
pyston/build/cpython_dbg_build/pyston: pyston/build/cpython_dbg_build/Makefile $(wildcard */*.c) $(wildcard */*.h) pyston/aot/aot_all.bc
	cd pyston/build/cpython_dbg_build; $(MAKE)
	touch $@ # some cpython .c files don't affect the python executable
pyston/build/cpython_stock_build/pyston: pyston/build/cpython_stock_build/Makefile $(wildcard */*.c) $(wildcard */*.h)
	cd pyston/build/cpython_stock_build; $(MAKE)
	touch $@ # some cpython .c files don't affect the python executable
pyston/build/cpython_stockunopt_build/pyston: pyston/build/cpython_stockunopt_build/Makefile $(wildcard */*.c) $(wildcard */*.h)
	cd pyston/build/cpython_stockunopt_build; $(MAKE)
	touch $@ # some cpython .c files don't affect the python executable
pyston/build/cpython_stockdbg_build/pyston: pyston/build/cpython_stockdbg_build/Makefile $(wildcard */*.c) $(wildcard */*.h)
	cd pyston/build/cpython_stockdbg_build; $(MAKE)
	touch $@ # some cpython .c files don't affect the python executable

pyston/build/cpython_bc_install/usr/bin/python3: pyston/build/cpython_bc_build/pyston
	cd pyston/build/cpython_bc_build; WRAPPER_REALCC=$(realpath $(CLANG)) WRAPPER_OUTPUT_PREFIX=../cpython_bc $(MAKE) install DESTDIR=$(abspath pyston/build/cpython_bc_install)
pyston/build/cpython_unopt_install/usr/bin/python3: pyston/build/cpython_unopt_build/pyston
	cd pyston/build/cpython_unopt_build; $(MAKE) install DESTDIR=$(abspath pyston/build/cpython_unopt_install)
pyston/build/cpython_opt_install/usr/bin/python3: pyston/build/cpython_opt_build/pyston
	cd pyston/build/cpython_opt_build; $(MAKE) install DESTDIR=$(abspath pyston/build/cpython_opt_install)
pyston/build/cpython_dbg_install/usr/bin/python3: pyston/build/cpython_dbg_build/pyston
	cd pyston/build/cpython_dbg_build; $(MAKE) install DESTDIR=$(abspath pyston/build/cpython_dbg_install)
pyston/build/cpython_stock_install/usr/bin/python3: pyston/build/cpython_stock_build/pyston
	cd pyston/build/cpython_stock_build; $(MAKE) install DESTDIR=$(abspath pyston/build/cpython_stock_install)
pyston/build/cpython_stockunopt_install/usr/bin/python3: pyston/build/cpython_stockunopt_build/pyston
	cd pyston/build/cpython_stockunopt_build; $(MAKE) install DESTDIR=$(abspath pyston/build/cpython_stockunopt_install)
pyston/build/cpython_stockdbg_install/usr/bin/python3: pyston/build/cpython_stockdbg_build/pyston
	cd pyston/build/cpython_stockdbg_build; $(MAKE) install DESTDIR=$(abspath pyston/build/cpython_stockdbg_install)

VIRTUALENV:=pyston/build/bootstrap_env/bin/virtualenv
$(VIRTUALENV):
	virtualenv -p python3 pyston/build/bootstrap_env
	pyston/build/bootstrap_env/bin/pip install virtualenv

pyston/build/bc_env/bin/python: pyston/build/cpython_bc_install/usr/bin/python3 | $(VIRTUALENV)
	$(VIRTUALENV) -p $< pyston/build/bc_env
pyston/build/unopt_env/bin/python: pyston/build/cpython_unopt_install/usr/bin/python3 | $(VIRTUALENV)
	$(VIRTUALENV) -p $< pyston/build/unopt_env
pyston/build/optnobolt_env/bin/python: pyston/build/cpython_opt_install/usr/bin/python3 | $(VIRTUALENV)
	$(VIRTUALENV) -p $< pyston/build/optnobolt_env
pyston/build/opt_env/bin/python: pyston/build/cpython_opt_install/usr/bin/python3.bolt | $(VIRTUALENV)
	$(VIRTUALENV) -p $< pyston/build/opt_env
pyston/build/stock_env/bin/python: pyston/build/cpython_stock_install/usr/bin/python3 | $(VIRTUALENV)
	$(VIRTUALENV) -p $< pyston/build/stock_env
pyston/build/stockunopt_env/bin/python: pyston/build/cpython_stockunopt_install/usr/bin/python3 | $(VIRTUALENV)
	$(VIRTUALENV) -p $< pyston/build/stockunopt_env
pyston/build/stockdbg_env/bin/python: pyston/build/cpython_stockdbg_install/usr/bin/python3 | $(VIRTUALENV)
	$(VIRTUALENV) -p $< pyston/build/stockdbg_env
pyston/build/dbg_env/bin/python: pyston/build/cpython_dbg_install/usr/bin/python3 | $(VIRTUALENV)
	$(VIRTUALENV) -p $< pyston/build/dbg_env
pyston/build/system_env/bin/python: | $(VIRTUALENV)
	$(VIRTUALENV) -p python3.8 pyston/build/system_env
	pyston/build/system_env/bin/pip install six pyperf cython || (rm -rf pyston/build/system_env; false)
pyston/build/systemdbg_env/bin/python: | $(VIRTUALENV)
	$(VIRTUALENV) -p python3.8-dbg pyston/build/systemdbg_env
pyston/build/pypy_env/bin/python: | $(VIRTUALENV)
	$(VIRTUALENV) -p pypy3 pyston/build/pypy_env

define make_bolt_rule
$(eval
$(2).fdata: $(1) $(BOLT) | $(VIRTUALENV)
	rm -rf /tmp/tmp_env
	rm $(2)*.fdata || true
	$(BOLT) $(1) -instrument -instrumentation-file-append-pid -instrumentation-file=$(abspath $(2)) -o $(1).bolt_inst
	$(VIRTUALENV) -p $(1).bolt_inst /tmp/tmp_env
	/tmp/tmp_env/bin/pip install -r pyston/benchmark_requirements.txt
	/tmp/tmp_env/bin/python3 pyston/run_profile_task.py
	$(MERGE_FDATA) $(2).*.fdata > $(2).fdata

$(2): $(2).fdata
	$(BOLT) $(1) -o $(2) -data=$(2).fdata -update-debug-sections -reorder-blocks=cache+ -reorder-functions=hfsort+ -split-functions=3 -icf=1 -inline-all -split-eh -reorder-functions-use-hot-size -peepholes=all -jump-tables=aggressive -inline-ap -indirect-call-promotion=all -dyno-stats -frame-opt=hot -use-gnu-stack
)
endef

$(call make_bolt_rule,pyston/build/cpython_unopt_install/usr/bin/python3,pyston/build/cpython_unopt_install/usr/bin/python3.bolt)
$(call make_bolt_rule,pyston/build/cpython_opt_install/usr/bin/python3,pyston/build/cpython_opt_install/usr/bin/python3.bolt)

.PHONY: cpython
cpython: pyston/build/bc_env/bin/python pyston/build/unopt_env/bin/python pyston/build/opt_env/bin/python



%.ll: %.bc $(LLVM_TOOLS)
	pyston/build/Release/llvm/bin/llvm-dis $<

.PRECIOUS: %.normalized_ll
%.normalized_ll: | %.bc
	pyston/build/Release/llvm/bin/opt -S -strip $| | sed -e 's/%[0-9]\+/%X/g' -e 's/@[0-9]\+/@X/g' > $@
find_similar_traces: $(patsubst %.bc,%.normalized_ll,$(wildcard pyston/aot/*.bc))
	python3 pyston/aot/aot_diff_ir.py

pyston/aot/aot_pre_trace.c: pyston/aot/aot_gen.py pyston/build/cpython_bc_install/usr/bin/python3
	cd pyston/aot; LD_LIBRARY_PATH="`pwd`/../build/Release/nitrous/:`pwd`/../build/Release/pystol/" ../build/cpython_bc_install/usr/bin/python3 aot_gen.py --action=pretrace
pyston/aot/aot_pre_trace.bc: pyston/aot/aot_pre_trace.c
	$(CLANG) -O2 -g -fPIC -Wno-incompatible-pointer-types -Wno-int-conversion $< -Ipyston/build/cpython_bc_install/usr/include/pyston3.8/ -Ipyston/build/cpython_bc_install/usr/include/pyston3.8/internal/ -Ipyston/nitrous/ -emit-llvm -c -o $@
pyston/aot/aot_pre_trace.so: pyston/aot/aot_pre_trace.c pyston/build/Release/nitrous/libinterp.so
	$(CLANG) -O2 -g -fPIC -Wno-incompatible-pointer-types -Wno-int-conversion $< -Ipyston/build/cpython_bc_install/usr/include/pyston3.8/ -Ipyston/build/cpython_bc_install/usr/include/pyston3.8/internal/ -Ipyston/nitrous/ -shared -Lpyston/build/Release/nitrous -linterp -o $@

pyston/aot/all.bc: pyston/build/cpython_bc_build/pyston $(LLVM_TOOLS) pyston/aot/aot_pre_trace.bc
	pyston/build/Release/llvm/bin/llvm-link $(filter-out %_testembed.o.bc %frozenmain.o.bc pyston/build/cpython_bc/Modules/%,$(wildcard pyston/build/cpython_bc/*/*.bc)) pyston/build/cpython_bc/Modules/gcmodule.o.bc pyston/build/cpython_bc/Modules/getpath.o.bc pyston/build/cpython_bc/Modules/main.o.bc pyston/build/cpython_bc/Modules/config.o.bc pyston/aot/aot_pre_trace.bc -o=$@

dbg_aot_trace: pyston/aot/all.bc pyston/aot/aot_pre_trace.so pyston/aot/aot_gen.py pyston/build/cpython_bc_install/usr/bin/python3 build_dbg
	cd pyston/aot; rm -f aot_module*.bc
	cd pyston/aot; LD_LIBRARY_PATH="`pwd`/../build/PartialDebug/nitrous/:`pwd`/../build/PartialDebug/pystol/" gdb --args ../build/cpython_bc_install/usr/bin/python3 aot_gen.py -vv --action=trace
	cd pyston/aot; ls -al aot_module*.bc | wc -l

ONLY?=null
aot_trace_only: pyston/aot/all.bc pyston/aot/aot_pre_trace.so pyston/aot/aot_gen.py pyston/build/cpython_bc_install/usr/bin/python3 pyston/build/Release/nitrous/libinterp.so pyston/build/Release/pystol/libpystol.so
	cd pyston/aot; LD_LIBRARY_PATH="`pwd`/../build/Release/nitrous/:`pwd`/../build/Release/pystol/" ../build/cpython_bc_install/usr/bin/python3 aot_gen.py --action=trace -vv --only=$(ONLY)

dbg_aot_trace_only: pyston/aot/all.bc pyston/aot/aot_pre_trace.so pyston/aot/aot_gen.py pyston/build/cpython_bc_install/usr/bin/python3 build_dbg
	cd pyston/aot; LD_LIBRARY_PATH="`pwd`/../build/PartialDebug/nitrous/:`pwd`/../build/PartialDebug/pystol/" gdb --ex run --args ../build/cpython_bc_install/usr/bin/python3 aot_gen.py --action=trace -vv --only=$(ONLY)

# Not really dependent on aot_profile.c, but aot_profile.c gets generated at the same time as the real dependencies
pyston/aot/aot_all.bc: pyston/aot/aot_profile.c
	cd pyston/aot; ../build/Release/llvm/bin/llvm-link aot_module*.bc -o aot_all.bc

# The "%"s here are to force make to consider these as group targets and not run this target multiple times
pyston/aot/aot_profile%c: pyston/aot/all.bc pyston/aot/aot_pre_trace.so pyston/build/cpython_bc_install/usr/bin/python3 pyston/aot/aot_gen.py pyston/build/Release/nitrous/libinterp.so pyston/build/Release/pystol/libpystol.so
	cd pyston/aot; rm -f aot_module*.bc
	cd pyston/aot; LD_LIBRARY_PATH="`pwd`/../build/Release/nitrous/:`pwd`/../build/Release/pystol/" ../build/cpython_bc_install/usr/bin/python3 aot_gen.py --action=trace
	cd pyston/aot; ls -al aot_module*.bc | wc -l

PYPERF:=pyston/build/system_env/bin/pyperf command -w 0 -l 1 -p 1 -n $(or $(N),$(N),3) -v --affinity 0
linktest:
	$(PYPERF) python3.8 nbody.py 3000
	$(PYPERF) ~/debian/python3.8-3.8.0/build-static/python nbody.py 3000
	$(PYPERF) ~/debian/extracted/usr/bin/python3.8 nbody.py 3000
	$(PYPERF) pyston/build/cpython_stock_build/python nbody.py 3000
	$(PYPERF) pyston/build/cpython_stock_build/python_linkopt nbody.py 3000
	$(PYPERF) pyston/build/cpython_opt_build/python_linkopt nbody.py 3000


BC_BENCH_ENV:=pyston/build/bc_env/update.stamp
DBG_BENCH_ENV:=pyston/build/dbg_env/update.stamp
UNOPT_BENCH_ENV:=pyston/build/unopt_env/update.stamp
OPTNOBOLT_BENCH_ENV:=pyston/build/optnobolt_env/update.stamp
OPT_BENCH_ENV:=pyston/build/opt_env/update.stamp
STOCK_BENCH_ENV:=pyston/build/stock_env/update.stamp
STOCKUNOPT_BENCH_ENV:=pyston/build/stockunopt_env/update.stamp
STOCKDBG_BENCH_ENV:=pyston/build/stockdbg_env/update.stamp
SYSTEM_BENCH_ENV:=pyston/build/system_env/update.stamp
SYSTEMDBG_BENCH_ENV:=pyston/build/systemdbg_env/update.stamp
PYPY_BENCH_ENV:=pyston/build/pypy_env/update.stamp
$(BC_BENCH_ENV): pyston/benchmark_requirements.txt pyston/benchmark_requirements_nonpypy.txt | pyston/build/bc_env/bin/python
	pyston/build/bc_env/bin/pip install -r pyston/benchmark_requirements.txt -r pyston/benchmark_requirements_nonpypy.txt
	touch $@
$(DBG_BENCH_ENV): pyston/benchmark_requirements.txt pyston/benchmark_requirements_nonpypy.txt | pyston/build/dbg_env/bin/python
	pyston/build/dbg_env/bin/pip install -r pyston/benchmark_requirements.txt -r pyston/benchmark_requirements_nonpypy.txt
	touch $@
$(UNOPT_BENCH_ENV): pyston/benchmark_requirements.txt pyston/benchmark_requirements_nonpypy.txt | pyston/build/unopt_env/bin/python
	pyston/build/unopt_env/bin/pip install -r pyston/benchmark_requirements.txt -r pyston/benchmark_requirements_nonpypy.txt
	touch $@
$(OPTNOBOLT_BENCH_ENV): pyston/benchmark_requirements.txt pyston/benchmark_requirements_nonpypy.txt | pyston/build/optnobolt_env/bin/python
	pyston/build/optnobolt_env/bin/pip install -r pyston/benchmark_requirements.txt -r pyston/benchmark_requirements_nonpypy.txt
	touch $@
$(OPT_BENCH_ENV): pyston/benchmark_requirements.txt pyston/benchmark_requirements_nonpypy.txt | pyston/build/opt_env/bin/python
	pyston/build/opt_env/bin/pip install -r pyston/benchmark_requirements.txt -r pyston/benchmark_requirements_nonpypy.txt
	touch $@
$(STOCK_BENCH_ENV): pyston/benchmark_requirements.txt pyston/benchmark_requirements_nonpypy.txt | pyston/build/stock_env/bin/python
	pyston/build/stock_env/bin/pip install -r pyston/benchmark_requirements.txt -r pyston/benchmark_requirements_nonpypy.txt
	touch $@
$(STOCKUNOPT_BENCH_ENV): pyston/benchmark_requirements.txt pyston/benchmark_requirements_nonpypy.txt | pyston/build/stockunopt_env/bin/python
	pyston/build/stockunopt_env/bin/pip install -r pyston/benchmark_requirements.txt -r pyston/benchmark_requirements_nonpypy.txt
	touch $@
$(STOCKDBG_BENCH_ENV): pyston/benchmark_requirements.txt pyston/benchmark_requirements_nonpypy.txt | pyston/build/stockdbg_env/bin/python
	pyston/build/stockdbg_env/bin/pip install -r pyston/benchmark_requirements.txt -r pyston/benchmark_requirements_nonpypy.txt
	touch $@
$(SYSTEM_BENCH_ENV): pyston/benchmark_requirements.txt pyston/benchmark_requirements_nonpypy.txt | pyston/build/system_env/bin/python
	pyston/build/system_env/bin/pip install -r pyston/benchmark_requirements.txt -r pyston/benchmark_requirements_nonpypy.txt
	touch $@
$(SYSTEMDBG_BENCH_ENV): pyston/benchmark_requirements.txt pyston/benchmark_requirements_nonpypy.txt | pyston/build/systemdbg_env/bin/python
	pyston/build/systemdbg_env/bin/pip install -r pyston/benchmark_requirements.txt -r pyston/benchmark_requirements_nonpypy.txt
	touch $@
$(PYPY_BENCH_ENV): pyston/benchmark_requirements.txt | pyston/build/pypy_env/bin/python
	pyston/build/pypy_env/bin/pip install -r pyston/benchmark_requirements.txt
	touch $@
update_envs:
	for i in bc unopt opt stock stockunopt stockdbg system systemdbg pypy; do pyston/build/$${i}_env/bin/pip install -r pyston/benchmark_requirements.txt & done; wait

%_bc: %.py $(BC_BENCH_ENV)
	time pyston/build/bc_env/bin/python3 $< $(ARGS)
%_unopt: %.py $(UNOPT_BENCH_ENV)
	time pyston/build/unopt_env/bin/python3 $< $(ARGS)
%_optnobolt: %.py $(OPTNOBOLT_BENCH_ENV)
	time pyston/build/optnobolt_env/bin/python3 $< $(ARGS)
%_opt: %.py $(OPT_BENCH_ENV)
	time pyston/build/opt_env/bin/python3 $< $(ARGS)
%_dbg: %.py $(DBG_BENCH_ENV)
	time pyston/build/dbg_env/bin/python3 $< $(ARGS)
%_stock: %.py $(STOCK_BENCH_ENV)
	time pyston/build/stock_env/bin/python3 $< $(ARGS)
%_stockunopt: %.py $(STOCKUNOPT_BENCH_ENV)
	time pyston/build/stockunopt_env/bin/python3 $< $(ARGS)
%_stockdbg: %.py $(STOCKDBG_BENCH_ENV)
	time pyston/build/stockdbg_env/bin/python3 $< $(ARGS)
%_system: %.py $(SYSTEM_BENCH_ENV)
	time pyston/build/system_env/bin/python3 $< $(ARGS)
%_system_dbg: %.py $(SYSTEMDBG_BENCH_ENV)
	time pyston/build/systemdbg_env/bin/python3 $< $(ARGS)
%_pypy: %.py $(PYPY_BENCH_ENV)
	time pyston/build/pypy_env/bin/python3 $< $(ARGS)


perf_report:
	perf report -n --no-children --objdump=pyston/tools/perf_jit.py
perf_%_unopt: %.py $(UNOPT_BENCH_ENV)
	JIT_PERF_MAP=1 perf record -g ./pyston/build/unopt_env/bin/python3 $< $(ARGS)
	$(MAKE) perf_report
perf_%_optnobolt: %.py $(OPTNOBOLT_BENCH_ENV)
	JIT_PERF_MAP=1 perf record -g ./pyston/build/optnobolt_env/bin/python3 $< $(ARGS)
	$(MAKE) perf_report
perf_% perf_%_opt: %.py $(OPT_BENCH_ENV)
	JIT_PERF_MAP=1 perf record -g ./pyston/build/opt_env/bin/python3 $< $(ARGS)
	$(MAKE) perf_report
perf_%_stock: %.py $(STOCK_BENCH_ENV)
	JIT_PERF_MAP=1 perf record -g ./pyston/build/stock_env/bin/python3 $< $(ARGS)
	$(MAKE) perf_report
perf_%_stockunopt: %.py $(STOCKUNOPT_BENCH_ENV)
	JIT_PERF_MAP=1 perf record -g ./pyston/build/stockunopt_env/bin/python3 $< $(ARGS)
	$(MAKE) perf_report
perf_%_system: %.py $(SYSTEM_BENCH_ENV)
	JIT_PERF_MAP=1 perf record -g ./pyston/build/system_env/bin/python3 $< $(ARGS)
	$(MAKE) perf_report

multi_unopt: pyston/run_profile_task_unopt
multi_opt: pyston/run_profile_task_opt
multi_stock: pyston/run_profile_task_stock
multi_stockunopt: pyston/run_profile_task_stockunopt
multi_system: pyston/run_profile_task_system
pyperf_multi_unopt: pyston/pyperf_run_profile_task_unopt
pyperf_multi_opt: pyston/pyperf_run_profile_task_opt
pyperf_multi_stock: pyston/pyperf_run_profile_task_stock
pyperf_multi_system: pyston/pyperf_run_profile_task_system
perf_multi_unopt: pyston/perf_run_profile_task_unopt
perf_multi_opt: pyston/perf_run_profile_task_opt

measure: $(OPT_BENCH_ENV) pyston/build/system_env/bin/python
	rm -f results.json
	@# Run the command a second time with output if it failed:
	$(MAKE) tune > /dev/null || $(MAKE) tune
	$(PYPERF) -n $(or $(N),$(N),5) -o results.json ./pyston/build/opt_env/bin/python3 pyston/run_profile_task.py
	$(MAKE) tune_reset > /dev/null
measure_%: %.py $(OPT_BENCH_ENV) pyston/build/system_env/bin/python
	rm -f results.json
	$(MAKE) tune > /dev/null || $(MAKE) tune
	$(PYPERF) -n $(or $(N),$(N),5) -o results.json ./pyston/build/opt_env/bin/python $<
	$(MAKE) tune_reset > /dev/null
measure_append: $(OPT_BENCH_ENV) pyston/build/system_env/bin/python
	$(MAKE) tune > /dev/null || $(MAKE) tune
	$(PYPERF) -n $(or $(N),$(N),1) --append results.json ./pyston/build/opt_env/bin/python3 pyston/run_profile_task.py
	$(MAKE) tune_reset > /dev/null
compare: pyston/build/system_env/bin/python
	pyston/build/system_env/bin/pyperf compare_to -v $(ARGS) results.json

pyperf_%_bc: %.py $(BC_BENCH_ENV) pyston/build/system_env/bin/python
	$(PYPERF) pyston/build/bc_env/bin/python3 $< $(ARGS)
pyperf_%_unopt: %.py $(UNOPT_BENCH_ENV) pyston/build/system_env/bin/python
	$(PYPERF) pyston/build/unopt_env/bin/python3 $< $(ARGS)
pyperf_%_optnobolt: %.py $(OPT_BENCH_ENV) pyston/build/system_env/bin/python
	$(PYPERF) pyston/build/optnobolt_env/bin/python3 $< $(ARGS)
pyperf_%_opt: %.py $(OPT_BENCH_ENV) pyston/build/system_env/bin/python
	$(PYPERF) pyston/build/opt_env/bin/python3 $< $(ARGS)
pyperf_%_stock: %.py $(STOCK_BENCH_ENV) pyston/build/system_env/bin/python
	$(PYPERF) pyston/build/stock_env/bin/python3 $< $(ARGS)
pyperf_%_stockunopt: %.py $(STOCKUNOPT_BENCH_ENV) pyston/build/system_env/bin/python
	$(PYPERF) pyston/build/stockunopt_env/bin/python3 $< $(ARGS)
pyperf_%_system: %.py $(SYSTEM_BENCH_ENV) pyston/build/system_env/bin/python
	$(PYPERF) pyston/build/system_env/bin/python3 $< $(ARGS)
pyperf_%_pypy: %.py $(PYPY_BENCH_ENV) pyston/build/system_env/bin/python
	$(PYPERF) pyston/build/pypy_env/bin/python3 $< $(ARGS)

dbg_%_unopt: %.py $(UNOPT_BENCH_ENV)
	gdb --args pyston/build/unopt_env/bin/python $< $(ARGS)
dbg_%: %.py $(DBG_BENCH_ENV)
	gdb --args pyston/build/dbg_env/bin/python $< $(ARGS)


# UNSAFE build target
# If you're making changes to cpython but don't want to rebuild the traces or anything beyond the python binary,
# you can call this target for a much faster build.
# Skips cpython_bc_build, aot_gen, and cpython_unopt_install
# If you want to run the skipped steps, you will have to touch a file before doing a (safe) rebuild
unsafe_unopt:
	$(MAKE) -C pyston/build/cpython_unopt_build
	/bin/cp pyston/build/cpython_unopt_build/pyston pyston/build/cpython_unopt_install/usr/bin/python3.8
unsafe_dbg:
	$(MAKE) -C pyston/build/cpython_dbg_build
	/bin/cp pyston/build/cpython_dbg_build/pyston pyston/build/cpython_dbg_install/usr/bin/python3.8
unsafe_% unsafe_%_unopt: %.py unsafe_unopt
	time pyston/build/unopt_env/bin/python3 $< $(ARGS)
unsafe_dbg_%: %.py unsafe_unopt
	gdb --args pyston/build/unopt_env/bin/python3 $< $(ARGS)
unsafe_perf_%: %.py unsafe_unopt
	JIT_PERF_MAP=1 perf record -g pyston/build/unopt_env/bin/python3 $< $(ARGS)
	$(MAKE) perf_report
unsafe_perfstat_%: %.py unsafe_unopt
	perf stat -ddd pyston/build/unopt_env/bin/python3 $< $(ARGS)
unsafe_pyperf_%: %.py unsafe_unopt
	$(PYPERF) pyston/build/unopt_env/bin/python3 $< $(ARGS)
unsafe_test: unsafe_unopt
	$(MAKE) _runtests

test_opt: build_release pyston/build/cpython_bc_build/pyston aot/all.bc
	LD_LIBRARY_PATH=pyston/build/Release/nitrous:pyston/build/Release/pystol pyston/build/cpython_bc_build/pyston -u pyston/tools/test_optimize.py test.c test
dbg_test_opt: build_dbg pyston/build/cpython_bc_build/pyston aot/all.bc
	LD_LIBRARY_PATH=pyston/build/PartialDebug/nitrous:pyston/build/PartialDebug/pystol gdb --args pyston/build/cpython_bc_build/pyston -u pyston/tools/test_optimize.py test.c test

.PHONY: test dbg_test _runtests test_%
_expected: python/test/external/test_django.expected python/test/external/test_six.expected python/test/external/test_requests.expected python/test/external/test_numpy.expected python/test/external/test_setuptools.expected python/test/external/test_urllib3.expected

# args: test_suffix test_binary
define make_test_build
$(eval
pyston/test/external/test_%.$(1): pyston/test/external/test_%.py $(2) | $(VIRTUALENV)
	OPENSSL_CONF=$(abspath pyston/test/openssl_dev.cnf) bash -c "set -o pipefail; $(2) -u $$< 2>&1 | tee $$@_"
	mv $$@_ $$@
)
endef

$(call make_test_build,expected,pyston/build/system_env/bin/python)
.PRECIOUS: pyston/test/external/test_%.expected pyston/test/external/test_%.output
$(call make_test_build,output,pyston/build/unopt_env/bin/python)

$(call make_test_build,dbgexpected,pyston/build/stockdbg_env/bin/python)
.PRECIOUS: pyston/test/external/test_%.dbgexpected pyston/test/external/test_%.dbgoutput
$(call make_test_build,dbgoutput,pyston/build/dbg_env/bin/python)

test_%: pyston/test/external/test_%.expected pyston/test/external/test_%.output
	pyston/build/system_env/bin/python pyston/test/external/helpers.py compare $< $(patsubst %.expected,%.output,$<)

testdbg_%: pyston/test/external/test_%.dbgexpected pyston/test/external/test_%.dbgoutput
	pyston/build/system_env/bin/python pyston/test/external/helpers.py compare $< $(patsubst %.dbgexpected,%.dbgoutput,$<)

cpython_testsuite: pyston/build/cpython_unopt_build/pyston
	OPENSSL_CONF=$(abspath pyston/test/openssl_dev.cnf) $(MAKE) -C pyston/build/cpython_unopt_build test

cpython_testsuite_dbg: pyston/build/cpython_dbg_build/pyston
	OPENSSL_CONF=$(abspath pyston/test/openssl_dev.cnf) $(MAKE) -C pyston/build/cpython_dbg_build test

# Note: cpython_testsuite is itself parallel so we might need to run it not in parallel
# with the other tests
_runtests: pyston/test/caches_unopt pyston/test/deferred_stack_decref_unopt pyston/test/getattr_caches_unopt test_django test_urllib3 test_setuptools test_six test_requests cpython_testsuite
_runtestsdbg: pyston/test/caches_dbg testdbg_django testdbg_urllib3 testdbg_setuptools testdbg_six testdbg_requests cpython_testsuite_dbg

test: pyston/build/system_env/bin/python pyston/build/unopt_env/bin/python
	rm -f $(wildcard pyston/test/external/*.output)
	# Test mostly tests the CAPI so don't rerun it with different JIT_MIN_RUNS
	# Note: test_numpy is internally parallel so we might have to re-separate it
	# into a separate step
	$(MAKE) _runtests test_numpy
	$(MAKE) test_numpy
	JIT_MAX_MEM=50000 pyston/build/unopt_env/bin/python pyston/test/jit_limit.py
	JIT_MAX_MEM=50000 pyston/build/unopt_env/bin/python pyston/test/jit_osr_limit.py
	pyston/build/unopt_env/bin/python pyston/test/test_venvs.py
	rm -f $(wildcard pyston/test/external/*.output)
	JIT_MIN_RUNS=0 $(MAKE) _runtests
	rm -f $(wildcard pyston/test/external/*.output)
	JIT_MIN_RUNS=50 $(MAKE) _runtests
	rm -f $(wildcard pyston/test/external/*.output)
	JIT_MIN_RUNS=9999999999 $(MAKE) _runtests
	rm -f $(wildcard pyston/test/external/*.output)

stocktest: pyston/build/cpython_stockunopt_build/python
	$(MAKE) -C pyston/build/cpython_stockunopt_build test
dbg_test: python/test/dbg_method_call_unopt

.PHONY: package
package:
ifeq ($(shell lsb_release -sr),16.04)
	# 16.04 needs this file but on newer ubuntu versions it will make it fail
	echo 10 > pyston/debian/compat
	cd pyston; DEB_BUILD_OPTIONS=nocheck dpkg-buildpackage -b -d
else
	cd pyston; DEB_BUILD_OPTIONS=nocheck dpkg-buildpackage --build=binary --no-sign --jobs=auto -d
endif

bench: $(OPT_BENCH_ENV) $(SYSTEM_BENCH_ENV)
	$(MAKE) -C pyston/tools/benchmarks_runner quick_analyze

full_bench: $(OPT_BENCH_ENV) $(SYSTEM_BENCH_ENV) $(PYPY_BENCH_ENV)
	$(MAKE) -C pyston/tools/benchmarks_runner analyze

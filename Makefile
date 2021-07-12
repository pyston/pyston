.SUFFIXES:

CLANG:=pyston/build/Release/llvm/bin/clang
GDB:=gdb
.DEFAULT_GOAL:=all

PYTHON_MAJOR:=3
PYTHON_MINOR:=8
PYSTON_MAJOR:=2
PYSTON_MINOR:=3

-include Makefile.local

.PHONY: all
all: pyston3

pyston3: pyston/build/opt_env/bin/python
	ln -sf $< $@

.PHONY: clean
clean:
	rm -rf pyston/build/*_env pyston/build/cpython_* pyston/build/aot*

tune: pyston/build/system_env/bin/python
	PYTHONPATH=pyston/tools pyston/build/system_env/bin/python -c "import tune; tune.tune()"
tune_reset: pyston/build/system_env/bin/python
	PYTHONPATH=pyston/tools pyston/build/system_env/bin/python -c "import tune; tune.untune()"

pyston/build/Release/build.ninja:
	mkdir -p pyston/build/Release
	@# Use gold linker since ld 2.32 (Ubuntu 19.04) is unable to link compiler-rt:
	cd pyston/build/Release; CC=clang CXX=clang++ cmake ../.. -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_USE_LINKER=gold -DLLVM_ENABLE_PROJECTS="clang;compiler-rt" -DLLVM_USE_PERF=ON -DCLANG_INCLUDE_TESTS=0 -DCOMPILER_RT_INCLUDE_TESTS=0 -DLLVM_INCLUDE_TESTS=0

pyston/build/PartialDebug/build.ninja:
	mkdir -p pyston/build/PartialDebug
	cd pyston/build/PartialDebug; CC=clang CXX=clang++ cmake ../.. -G Ninja -DCMAKE_BUILD_TYPE=PartialDebug -DLLVM_ENABLE_PROJECTS=clang -DLLVM_USE_PERF=ON -DCLANG_INCLUDE_TESTS=0 -DCOMPILER_RT_INCLUDE_TESTS=0 -DLLVM_INCLUDE_TESTS=0

pyston/build/Debug/build.ninja:
	mkdir -p pyston/build/Debug
	cd pyston/build/Debug; CC=clang CXX=clang++ cmake ../.. -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLLVM_ENABLE_PROJECTS=clang -DLLVM_USE_PERF=ON -DCLANG_INCLUDE_TESTS=0 -DCOMPILER_RT_INCLUDE_TESTS=0 -DLLVM_INCLUDE_TESTS=0

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

BOLT:=pyston/build/bolt/bin/llvm-bolt
PERF2BOLT:=pyston/build/bolt/bin/perf2bolt
MERGE_FDATA:=pyston/build/bolt/bin/merge-fdata
pyston/build/bolt/build.ninja:
	mkdir -p pyston/build/bolt
	cd pyston/build/bolt; cmake -G Ninja ../../bolt/bolt/llvm -DLLVM_TARGETS_TO_BUILD="X86" -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_ASSERTIONS=ON -DLLVM_INCLUDE_TESTS=0 -DLLVM_ENABLE_PROJECTS=bolt
bolt: $(BOLT)
$(BOLT): pyston/build/bolt/build.ninja
	cd pyston/build/bolt; ninja llvm-bolt merge-fdata perf2bolt

# this flags are what the default debian/ubuntu cpython uses
CPYTHON_EXTRA_CFLAGS:=-fstack-protector -specs=$(CURDIR)/pyston/tools/no-pie-compile.specs -D_FORTIFY_SOURCE=2
CPYTHON_EXTRA_LDFLAGS:=-specs=$(CURDIR)/pyston/tools/no-pie-link.specs -Wl,-z,relro

PROFILE_TASK:=../../../Lib/test/regrtest.py -j 1 -unone,decimal -x test_posix test_asyncio test_cmd_line_script test_compiler test_concurrent_futures test_ctypes test_dbm_dumb test_dbm_ndbm test_distutils test_ensurepip test_ftplib test_gdb test_httplib test_imaplib test_ioctl test_linuxaudiodev test_multiprocessing test_nntplib test_ossaudiodev test_poplib test_pydoc test_signal test_socket test_socketserver test_ssl test_subprocess test_sundry test_thread test_threaded_import test_threadedtempfile test_threading test_threading_local test_threadsignals test_venv test_zipimport_support || true

MAKEFILE_DEPENDENCIES:=Makefile.pre.in configure
pyston/build/cpython_bc_build/Makefile: $(CLANG) $(MAKEFILE_DEPENDENCIES)
	mkdir -p pyston/build/cpython_bc_build
	cd pyston/build/cpython_bc_build; WRAPPER_REALCC=$(realpath $(CLANG)) WRAPPER_OUTPUT_PREFIX=../cpython_bc CC=../../../pyston/clang_wrapper.py CFLAGS_NODIST="$(CPYTHON_EXTRA_CFLAGS) -Wno-unused-command-line-argument" LDFLAGS_NODIST="$(CPYTHON_EXTRA_LDFLAGS)" ../../../configure --prefix=/usr --disable-aot --disable-debugging-features --enable-configure

pyston/build/cpython_bc_build/pyston: pyston/build/cpython_bc_build/Makefile $(filter-out $(wildcard Python/aot*.c),$(wildcard */*.c)) $(wildcard */*.h)
	cd pyston/build/cpython_bc_build; WRAPPER_REALCC=$(realpath $(CLANG)) WRAPPER_OUTPUT_PREFIX=../cpython_bc $(MAKE)
	touch $@ # some cpython .c files don't affect the python executable

pyston/build/cpython_bc_install/usr/bin/python3: pyston/build/cpython_bc_build/pyston
	cd pyston/build/cpython_bc_build; WRAPPER_REALCC=$(realpath $(CLANG)) WRAPPER_OUTPUT_PREFIX=../cpython_bc $(MAKE) install DESTDIR=$(abspath pyston/build/cpython_bc_install)

VIRTUALENV:=pyston/build/bootstrap_env/bin/virtualenv
$(VIRTUALENV):
	virtualenv -p python3 pyston/build/bootstrap_env
	pyston/build/bootstrap_env/bin/pip install virtualenv

pyston/build/bc_env/bin/python: pyston/build/cpython_bc_install/usr/bin/python3 | $(VIRTUALENV)
	$(VIRTUALENV) -p $< pyston/build/bc_env
bc: pyston/build/bc_env/bin/python
pyston/build/system_env/bin/python: | $(VIRTUALENV)
	$(VIRTUALENV) -p python$(PYTHON_MAJOR).$(PYTHON_MINOR) pyston/build/system_env
	pyston/build/system_env/bin/pip install six pyperf cython || (rm -rf pyston/build/system_env; false)
pyston/build/systemdbg_env/bin/python: | $(VIRTUALENV)
	$(VIRTUALENV) -p python$(PYTHON_MAJOR).$(PYTHON_MINOR)-dbg pyston/build/systemdbg_env
pyston/build/pypy_env/bin/python: | $(VIRTUALENV)
	$(VIRTUALENV) -p pypy3 pyston/build/pypy_env

# Usage:
# $(call make_cpython_build,NAME,CONFIGURE_LINE,AOT_DEPENDENCY,DESTDIR,BOLT:[|binary|so])
define make_cpython_build
$(eval

pyston/build/cpython_$(1)_build/Makefile: $(MAKEFILE_DEPENDENCIES)
	mkdir -p pyston/build/cpython_$(1)_build
	cd pyston/build/cpython_$(1)_build; $(2)

pyston/build/cpython_$(1)_build/pyston: pyston/build/cpython_$(1)_build/Makefile $(wildcard */*.c) $(wildcard */*.h) $(3)
	cd pyston/build/cpython_$(1)_build; $$(MAKE)
	touch $$@ # some cpython .c files don't affect the python executable

pyston/build/cpython_$(1)_install/usr/bin/python3: pyston/build/cpython_$(1)_build/pyston
	cd pyston/build/cpython_$(1)_build; $$(MAKE) install DESTDIR=$(4) $(if $(findstring so,$(5)),INSTSONAME=libpython$(PYTHON_MAJOR).$(PYTHON_MINOR)-pyston$(PYSTON_MAJOR).$(PYSTON_MINOR).so.1.0.prebolt)

$(1): pyston/build/$(1)_env/bin/python

ifeq ($(5),binary)

pyston/build/cpython_$(1)_install/usr/bin/python3.bolt.fdata: pyston/build/cpython_$(1)_install/usr/bin/python3 $(BOLT) | $(VIRTUALENV)
	rm -rf /tmp/tmp_env_$(1)
	rm $$<.bolt*.fdata || true
	$(BOLT) $$< -instrument -instrumentation-file-append-pid -instrumentation-file=$$(abspath pyston/build/cpython_$(1)_install/usr/bin/python3.bolt) -o $$<.bolt_inst
	$(VIRTUALENV) -p $$<.bolt_inst /tmp/tmp_env_$(1)
	/tmp/tmp_env_$(1)/bin/pip install -r pyston/pgo_requirements.txt
	/tmp/tmp_env_$(1)/bin/python3 pyston/run_profile_task.py
	$(MERGE_FDATA) $$<.*.fdata > $$@

pyston/build/cpython_$(1)_install/usr/bin/python3.bolt: pyston/build/cpython_$(1)_install/usr/bin/python3.bolt.fdata
	$(BOLT) pyston/build/cpython_$(1)_install/usr/bin/python3 -o $$@ -data=$$< -update-debug-sections -reorder-blocks=cache+ -reorder-functions=hfsort+ -split-functions=3 -icf=1 -inline-all -split-eh -reorder-functions-use-hot-size -peepholes=all -jump-tables=aggressive -inline-ap -indirect-call-promotion=all -dyno-stats -frame-opt=hot -use-gnu-stack

pyston/build/$(1)_env/bin/python: pyston/build/cpython_$(1)_install/usr/bin/python3.bolt | $(VIRTUALENV)
	$(VIRTUALENV) -p $$< pyston/build/$(1)_env

else

pyston/build/$(1)_env/bin/python: pyston/build/cpython_$(1)_install/usr/bin/python3 | $(VIRTUALENV)
	LD_LIBRARY_PATH=$${LD_LIBRARY_PATH}:$$(abspath pyston/build/cpython_$(1)_install/usr/lib) $(VIRTUALENV) -p $$< pyston/build/$(1)_env

endif

ifeq ($(5),so)

# Bolt-on-.so strategy:
# We install the .so as libpython.so.prebolt,
# then LD_PRELOAD it into the pyston executable to force it to load,
# then run our perf task,
# then output the bolt'd file to the proper place
pyston/build/cpython_$(1)_install/usr/lib/libpython$(PYTHON_MAJOR).$(PYTHON_MINOR)-pyston$(PYSTON_MAJOR).$(PYSTON_MINOR).so.1.0.fdata: pyston/build/cpython_$(1)_install/usr/bin/python3
	rm -rf /tmp/tmp_env_$(1)
	LD_LIBRARY_PATH=$${LD_LIBRARY_PATH}:$$(abspath pyston/build/cpython_$(1)_install/usr/lib) LD_PRELOAD=libpython$(PYTHON_MAJOR).$(PYTHON_MINOR)-pyston$(PYSTON_MAJOR).$(PYSTON_MINOR).so.1.0.prebolt $(VIRTUALENV) -p $$< /tmp/tmp_env_$(1)
	LD_LIBRARY_PATH=$${LD_LIBRARY_PATH}:$$(abspath pyston/build/cpython_$(1)_install/usr/lib) LD_PRELOAD=libpython$(PYTHON_MAJOR).$(PYTHON_MINOR)-pyston$(PYSTON_MAJOR).$(PYSTON_MINOR).so.1.0.prebolt /tmp/tmp_env_$(1)/bin/pip install -r pyston/pgo_requirements.txt
	LD_LIBRARY_PATH=$${LD_LIBRARY_PATH}:$$(abspath pyston/build/cpython_$(1)_install/usr/lib) LD_PRELOAD=libpython$(PYTHON_MAJOR).$(PYTHON_MINOR)-pyston$(PYSTON_MAJOR).$(PYSTON_MINOR).so.1.0.prebolt perf record -e cycles:u -j any,u -o $$(patsubst %.fdata,%.perf,$$@) -- /tmp/tmp_env_$(1)/bin/python3 pyston/run_profile_task.py
	$(PERF2BOLT) -p $$(patsubst %.fdata,%.perf,$$@) -o $$@ pyston/build/cpython_$(1)_install/usr/lib/libpython$(PYTHON_MAJOR).$(PYTHON_MINOR)-pyston$(PYSTON_MAJOR).$(PYSTON_MINOR).so.1.0.prebolt

pyston/build/cpython_$(1)_install/usr/lib/libpython$(PYTHON_MAJOR).$(PYTHON_MINOR)-pyston$(PYSTON_MAJOR).$(PYSTON_MINOR).so.1.0: pyston/build/cpython_$(1)_install/usr/lib/libpython$(PYTHON_MAJOR).$(PYTHON_MINOR)-pyston$(PYSTON_MAJOR).$(PYSTON_MINOR).so.1.0.fdata
	$(BOLT) pyston/build/cpython_$(1)_install/usr/lib/libpython$(PYTHON_MAJOR).$(PYTHON_MINOR)-pyston$(PYSTON_MAJOR).$(PYSTON_MINOR).so.1.0.prebolt -o $$@ -data=$$< -update-debug-sections -reorder-blocks=cache+ -reorder-functions=hfsort+ -split-functions=3 -icf=1 -inline-all -split-eh -reorder-functions-use-hot-size -peepholes=all -jump-tables=aggressive -inline-ap -indirect-call-promotion=all -dyno-stats -frame-opt=hot -use-gnu-stack -jump-tables=none
	ln -sf $(abspath $$@) pyston/build/cpython_$(1)_install/usr/lib/libpython$(PYTHON_MAJOR).$(PYTHON_MINOR)-pyston$(PYSTON_MAJOR).$(PYSTON_MINOR).so

pyston/build/$(1)_env/bin/python: pyston/build/cpython_$(1)_install/usr/lib/libpython$(PYTHON_MAJOR).$(PYTHON_MINOR)-pyston$(PYSTON_MAJOR).$(PYSTON_MINOR).so.1.0

else ifneq ($(findstring enable-shared,$(2)),)

pyston/build/cpython_$(1)_install/usr/lib/libpython$(PYTHON_MAJOR).$(PYTHON_MINOR)-pyston$(PYSTON_MAJOR).$(PYSTON_MINOR).so.1.0: pyston/build/cpython_$(1)_install/usr/bin/python3

endif

pyston/build/$(1)_env/update.stamp: pyston/benchmark_requirements.txt pyston/benchmark_requirements_nonpypy.txt | pyston/build/$(1)_env/bin/python
	LD_LIBRARY_PATH=$${LD_LIBRARY_PATH}:$(abspath pyston/build/cpython_$(1)_install/usr/lib) pyston/build/$(1)_env/bin/pip install -r pyston/benchmark_requirements.txt -r pyston/benchmark_requirements_nonpypy.txt
	touch $$@

%_$(1): %.py pyston/build/$(1)_env/update.stamp
	LD_LIBRARY_PATH=$${LD_LIBRARY_PATH}:$(abspath pyston/build/cpython_$(1)_install/usr/lib) time pyston/build/$(1)_env/bin/python3 $$< $$(ARGS)

perf_%_$(1): %.py pyston/build/$(1)_env/update.stamp
	LD_LIBRARY_PATH=$${LD_LIBRARY_PATH}:$(abspath pyston/build/cpython_$(1)_install/usr/lib) JIT_PERF_MAP=1 perf record -g ./pyston/build/$(1)_env/bin/python3 $$< $$(ARGS)
	$$(MAKE) perf_report

pyperf_%_$(1): %.py ./pyston/build/$(1)_env/bin/update.stamp pyston/build/system_env/bin/python
	LD_LIBRARY_PATH=$${LD_LIBRARY_PATH}:$(abspath pyston/build/cpython_$(1)_install/usr/lib) $$(PYPERF) pyston/build/$(1)_env/bin/python3 $$< $$(ARGS)
)

# UNSAFE build target
# If you're making changes to cpython but don't want to rebuild the traces or anything beyond the python binary,
# you can call this target for a much faster build.
# Skips cpython_bc_build, aot_gen, and cpython_$(1)_install
# If you want to run the skipped steps, you will have to touch a file before doing a (safe) rebuild
unsafe_$(1):
	$(MAKE) -C pyston/build/cpython_$(1)_build
	/bin/cp pyston/build/cpython_$(1)_build/pyston pyston/build/cpython_$(1)_install/usr/bin/python$(PYTHON_MAJOR).$(PYTHON_MINOR)

endef

SO_IFNOT_AMD := $(if $(findstring AMD,$(shell cat /proc/cpuinfo)),,so)

COMMA:=,
$(call make_cpython_build,unopt,CC=gcc CFLAGS_NODIST="$(CPYTHON_EXTRA_CFLAGS) -fno-reorder-blocks-and-partition" LDFLAGS_NODIST="$(CPYTHON_EXTRA_LDFLAGS) -Wl$(COMMA)--emit-relocs" ../../../configure --prefix=$(abspath pyston/build/cpython_unopt_install/usr) --disable-debugging-features --enable-configure,pyston/build/aot/aot_all.bc,,)
$(call make_cpython_build,unoptshared,CC=gcc CFLAGS_NODIST="$(CPYTHON_EXTRA_CFLAGS) -fno-reorder-blocks-and-partition" LDFLAGS_NODIST="$(CPYTHON_EXTRA_LDFLAGS) -Wl$(COMMA)--emit-relocs" ../../../configure --prefix=$(abspath pyston/build/cpython_unoptshared_install/usr) --disable-debugging-features --enable-configure --enable-shared,pyston/build/aot_pic/aot_all.bc,,)
$(call make_cpython_build,opt,PROFILE_TASK="$(PROFILE_TASK)" CC=gcc CFLAGS_NODIST="$(CPYTHON_EXTRA_CFLAGS) -fno-reorder-blocks-and-partition" LDFLAGS_NODIST="$(CPYTHON_EXTRA_LDFLAGS) -Wl$(COMMA)--emit-relocs" ../../../configure --prefix=$(abspath pyston/build/cpython_opt_install/usr) --enable-optimizations --with-lto --disable-debugging-features --enable-configure,pyston/build/aot/aot_all.bc,,binary)
$(call make_cpython_build,optshared,PROFILE_TASK="$(PROFILE_TASK)" CC=gcc CFLAGS_NODIST="$(CPYTHON_EXTRA_CFLAGS) -fno-reorder-blocks-and-partition" LDFLAGS_NODIST="$(CPYTHON_EXTRA_LDFLAGS) -Wl$(COMMA)--emit-relocs" ../../../configure --prefix=$(abspath pyston/build/cpython_optshared_install/usr) --enable-optimizations --with-lto --disable-debugging-features --enable-shared --enable-configure,pyston/build/aot_pic/aot_all.bc,,$(SO_IFNOT_AMD))
$(call make_cpython_build,releaseunopt,PROFILE_TASK="$(PROFILE_TASK)" CC=gcc CFLAGS_NODIST="$(CPYTHON_EXTRA_CFLAGS) -fno-reorder-blocks-and-partition" LDFLAGS_NODIST="$(CPYTHON_EXTRA_LDFLAGS) -Wl$(COMMA)--emit-relocs" ../../../configure --prefix=/usr --disable-debugging-features --enable-configure,pyston/build/aot/aot_all.bc,$(abspath pyston/build/cpython_releaseunopt_install))
$(call make_cpython_build,releaseunoptshared,PROFILE_TASK="$(PROFILE_TASK)" CC=gcc CFLAGS_NODIST="$(CPYTHON_EXTRA_CFLAGS) -fno-reorder-blocks-and-partition" LDFLAGS_NODIST="$(CPYTHON_EXTRA_LDFLAGS) -Wl$(COMMA)--emit-relocs" ../../../configure --prefix=/usr --disable-debugging-features --enable-shared --enable-configure,pyston/build/aot_pic/aot_all.bc,$(abspath pyston/build/cpython_releaseunoptshared_install))
$(call make_cpython_build,release,      PROFILE_TASK="$(PROFILE_TASK)" CC=gcc CFLAGS_NODIST="$(CPYTHON_EXTRA_CFLAGS) -fno-reorder-blocks-and-partition" LDFLAGS_NODIST="$(CPYTHON_EXTRA_LDFLAGS) -Wl$(COMMA)--emit-relocs" ../../../configure --prefix=/usr --enable-optimizations --with-lto --disable-debugging-features --enable-configure,pyston/build/aot/aot_all.bc,$(abspath pyston/build/cpython_release_install),binary)
$(call make_cpython_build,releaseshared,PROFILE_TASK="$(PROFILE_TASK)" CC=gcc CFLAGS_NODIST="$(CPYTHON_EXTRA_CFLAGS) -fno-reorder-blocks-and-partition" LDFLAGS_NODIST="$(CPYTHON_EXTRA_LDFLAGS) -Wl$(COMMA)--emit-relocs" ../../../configure --prefix=/usr --enable-optimizations --with-lto --disable-debugging-features --enable-shared --enable-configure,pyston/build/aot_pic/aot_all.bc,$(abspath pyston/build/cpython_releaseshared_install),so)
# We have to --disable-debugging-features for consistency with the bc build
# If we had a separate bc-dbg build then we could change this
$(call make_cpython_build,dbg,CC=gcc CFLAGS_NODIST="$(CPYTHON_EXTRA_CFLAGS) -fno-reorder-blocks-and-partition" LDFLAGS_NODIST="$(CPYTHON_EXTRA_LDFLAGS) -Wl$(COMMA)--emit-relocs" ../../../configure --prefix=$(abspath pyston/build/cpython_dbg_install/usr) --with-pydebug --disable-debugging-features --enable-configure,pyston/build/aot/aot_all.bc)
$(call make_cpython_build,dbgshared,CC=gcc CFLAGS_NODIST="$(CPYTHON_EXTRA_CFLAGS) -fno-reorder-blocks-and-partition" LDFLAGS_NODIST="$(CPYTHON_EXTRA_LDFLAGS) -Wl$(COMMA)--emit-relocs" ../../../configure --prefix=$(abspath pyston/build/cpython_dbgshared_install/usr) --with-pydebug --disable-debugging-features --enable-shared --enable-configure,pyston/build/aot_pic/aot_all.bc)
$(call make_cpython_build,stock,PROFILE_TASK="$(PROFILE_TASK) || true" CC=gcc CFLAGS_NODIST="$(CPYTHON_EXTRA_CFLAGS)" LDFLAGS_NODIST="$(CPYTHON_EXTRA_LDFLAGS)" ../../../configure --prefix=$(abspath pyston/build/cpython_stock_install/usr) --enable-optimizations --with-lto --disable-pyston --enable-configure,)
$(call make_cpython_build,stockunopt,CC=gcc CFLAGS_NODIST="$(CPYTHON_EXTRA_CFLAGS)" LDFLAGS_NODIST="$(CPYTHON_EXTRA_LDFLAGS)" ../../../configure --prefix=$(abspath pyston/build/cpython_stockunopt_install/usr) --disable-pyston --enable-configure,)
$(call make_cpython_build,stockdbg,CC=gcc CFLAGS_NODIST="$(CPYTHON_EXTRA_CFLAGS)" LDFLAGS_NODIST="$(CPYTHON_EXTRA_LDFLAGS)" ../../../configure --prefix=$(abspath pyston/build/cpython_stockdbg_install/usr) --disable-pyston --with-pydebug --enable-configure,)

# Usage: $(call combine_builds,NAME)
define combine_builds
$(eval
pyston/build/cpython_$(1)_install/usr/lib/libpython$(PYTHON_MAJOR).$(PYTHON_MINOR)-pyston$(PYSTON_MAJOR).$(PYSTON_MINOR).so.1.0: pyston/build/cpython_$(1)shared_install/usr/lib/libpython$(PYTHON_MAJOR).$(PYTHON_MINOR)-pyston$(PYSTON_MAJOR).$(PYSTON_MINOR).so.1.0 | pyston/build/cpython_$(1)_install/usr/bin/python3
	bash -c "cp pyston/build/cpython_$(1){shared,}_install/usr/lib/python$(PYTHON_MAJOR).$(PYTHON_MINOR)-pyston$(PYSTON_MAJOR).$(PYSTON_MINOR)/_sysconfigdata__linux_x86_64-linux-gnu.py"
	bash -c "cp pyston/build/cpython_$(1){shared,}_install/usr/lib/libpython$(PYTHON_MAJOR).$(PYTHON_MINOR)-pyston$(PYSTON_MAJOR).$(PYSTON_MINOR).so.1.0"
	bash -c "cp -P pyston/build/cpython_$(1){shared,}_install/usr/lib/libpython$(PYTHON_MAJOR).$(PYTHON_MINOR)-pyston$(PYSTON_MAJOR).$(PYSTON_MINOR).so"
pyston/build/$(1)_env/bin/python: pyston/build/cpython_$(1)_install/usr/lib/libpython$(PYTHON_MAJOR).$(PYTHON_MINOR)-pyston$(PYSTON_MAJOR).$(PYSTON_MINOR).so.1.0
)
endef

$(call combine_builds,unopt)
$(call combine_builds,releaseunopt)
$(call combine_builds,opt)
$(call combine_builds,release)
$(call combine_builds,dbg)

.PHONY: cpython
cpython: pyston/build/bc_env/bin/python pyston/build/unopt_env/bin/python pyston/build/opt_env/bin/python



%.ll: %.bc $(LLVM_TOOLS)
	pyston/build/Release/llvm/bin/llvm-dis $<

.PRECIOUS: %.normalized_ll
%.normalized_ll: | %.bc
	pyston/build/Release/llvm/bin/opt -S -strip $| | sed -e 's/%[0-9]\+/%X/g' -e 's/@[0-9]\+/@X/g' > $@
find_similar_traces: $(patsubst %.bc,%.normalized_ll,$(wildcard pyston/aot/*.bc))
	python3 pyston/aot/aot_diff_ir.py

ONLY?=null

# Usage:
# $(call make_aot_build,NAME,FLAGS)
define make_aot_build
$(eval
pyston/build/$(1)/aot_pre_trace.c: pyston/aot/aot_gen.py pyston/build/cpython_bc_install/usr/bin/python3
	mkdir -p pyston/build/$(1)
	cd pyston/aot; LD_LIBRARY_PATH="`pwd`/../Release/nitrous/:`pwd`/../Release/pystol/" ../build/cpython_bc_install/usr/bin/python3 aot_gen.py --action=pretrace -o $$(abspath $$@) $(2)
pyston/build/$(1)/aot_pre_trace.bc: pyston/build/$(1)/aot_pre_trace.c
	$(CLANG) -O2 -g -fPIC -Wno-incompatible-pointer-types -Wno-int-conversion $$< -Ipyston/build/cpython_bc_install/usr/include/python$(PYTHON_MAJOR).$(PYTHON_MINOR)-pyston$(PYSTON_MAJOR).$(PYSTON_MINOR)/ -Ipyston/build/cpython_bc_install/usr/include/python$(PYTHON_MAJOR).$(PYTHON_MINOR)-pyston$(PYSTON_MAJOR).$(PYSTON_MINOR)/internal/ -Ipyston/nitrous/ -emit-llvm -c -o $$@
pyston/build/$(1)/aot_pre_trace.so: pyston/build/$(1)/aot_pre_trace.c pyston/build/Release/nitrous/libinterp.so
	$(CLANG) -O2 -g -fPIC -Wno-incompatible-pointer-types -Wno-int-conversion $$< -Ipyston/build/cpython_bc_install/usr/include/python$(PYTHON_MAJOR).$(PYTHON_MINOR)-pyston$(PYSTON_MAJOR).$(PYSTON_MINOR)/ -Ipyston/build/cpython_bc_install/usr/include/python$(PYTHON_MAJOR).$(PYTHON_MINOR)-pyston$(PYSTON_MAJOR).$(PYSTON_MINOR)/internal/ -Ipyston/nitrous/ -shared -Lpyston/build/Release/nitrous -linterp -o $$@

pyston/build/$(1)/all.bc: pyston/build/cpython_bc_build/pyston $(LLVM_TOOLS) pyston/build/$(1)/aot_pre_trace.bc
	pyston/build/Release/llvm/bin/llvm-link $$(filter-out %_testembed.o.bc %frozenmain.o.bc pyston/build/cpython_bc/Modules/%,$$(wildcard pyston/build/cpython_bc/*/*.bc)) pyston/build/cpython_bc/Modules/gcmodule.o.bc pyston/build/cpython_bc/Modules/getpath.o.bc pyston/build/cpython_bc/Modules/main.o.bc pyston/build/cpython_bc/Modules/config.o.bc pyston/build/$(1)/aot_pre_trace.bc -o=$$@

# Not really dependent on aot_profile.c, but aot_profile.c gets generated at the same time as the real dependencies
pyston/build/$(1)/aot_all.bc: pyston/build/$(1)/aot_profile.c
	cd pyston/build/$(1); ../Release/llvm/bin/llvm-link aot_module*.bc -o aot_all.bc

pyston/build/$(1)/aot_profile.c: pyston/build/$(1)/all.bc pyston/build/$(1)/aot_pre_trace.so pyston/build/cpython_bc_install/usr/bin/python3 pyston/aot/aot_gen.py pyston/build/Release/nitrous/libinterp.so pyston/build/Release/pystol/libpystol.so
	cd pyston/build/$(1); rm -f aot_module*.bc
	cd pyston/build/$(1); LD_LIBRARY_PATH="`pwd`/../Release/nitrous/:`pwd`/../Release/pystol/" ../cpython_bc_install/usr/bin/python3 ../../aot/aot_gen.py --action=trace $(2)
	cd pyston/build/$(1); ls -al aot_module*.bc | wc -l
)
endef

$(call make_aot_build,aot,)
$(call make_aot_build,aot_pic,--pic)
$(call make_aot_build,aot_dev,)

dbg_aot_trace: pyston/build/aot_dev/all.bc pyston/build/aot_dev/aot_pre_trace.so pyston/aot/aot_gen.py pyston/build/cpython_bc_install/usr/bin/python3 build_dbg
	cd pyston/build/aot_dev; rm -f aot_module*.bc
	cd pyston/build/aot_dev; LD_LIBRARY_PATH="`pwd`/../PartialDebug/nitrous/:`pwd`/../PartialDebug/pystol/" gdb --args ../cpython_bc_install/usr/bin/python3 ../../aot/aot_gen.py -vv --action=trace
	cd pyston/build/aot_dev; ls -al aot_module*.bc | wc -l

aot_trace_only: pyston/build/aot_dev/all.bc pyston/build/aot_dev/aot_pre_trace.so pyston/aot/aot_gen.py pyston/build/cpython_bc_install/usr/bin/python3 pyston/build/Release/nitrous/libinterp.so pyston/build/Release/pystol/libpystol.so
	cd pyston/build/aot_dev; LD_LIBRARY_PATH="`pwd`/../Release/nitrous/:`pwd`/../Release/pystol/" ../cpython_bc_install/usr/bin/python3 ../../aot/aot_gen.py --action=trace -vv --only=$(ONLY)

dbg_aot_trace_only: pyston/build/aot_dev/all.bc pyston/build/aot_dev/aot_pre_trace.so pyston/aot/aot_gen.py pyston/build/cpython_bc_install/usr/bin/python3 build_dbg
	cd pyston/build/aot_dev; LD_LIBRARY_PATH="`pwd`/../PartialDebug/nitrous/:`pwd`/../PartialDebug/pystol/" gdb --ex run --args ../cpython_bc_install/usr/bin/python3 ../../aot/aot_gen.py --action=trace -vv --only=$(ONLY)

PYPERF:=pyston/build/system_env/bin/pyperf command -w 0 -l 1 -p 1 -n $(or $(N),$(N),3) -v --affinity 0


BC_BENCH_ENV:=pyston/build/bc_env/update.stamp
SYSTEM_BENCH_ENV:=pyston/build/system_env/update.stamp
SYSTEMDBG_BENCH_ENV:=pyston/build/systemdbg_env/update.stamp
PYPY_BENCH_ENV:=pyston/build/pypy_env/update.stamp
$(BC_BENCH_ENV): pyston/benchmark_requirements.txt pyston/benchmark_requirements_nonpypy.txt | pyston/build/bc_env/bin/python
	pyston/build/bc_env/bin/pip install -r pyston/benchmark_requirements.txt -r pyston/benchmark_requirements_nonpypy.txt
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
%_system: %.py $(SYSTEM_BENCH_ENV)
	time pyston/build/system_env/bin/python3 $< $(ARGS)
%_system_dbg: %.py $(SYSTEMDBG_BENCH_ENV)
	time pyston/build/systemdbg_env/bin/python3 $< $(ARGS)
%_pypy: %.py $(PYPY_BENCH_ENV)
	time pyston/build/pypy_env/bin/python3 $< $(ARGS)


perf_report:
	perf report -n --no-children --objdump=pyston/tools/perf_jit.py
perf_%: perf_%_opt
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

OPT_BENCH_ENV:=pyston/build/opt_env/update.stamp

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
pyperf_%_system: %.py $(SYSTEM_BENCH_ENV) pyston/build/system_env/bin/python
	$(PYPERF) pyston/build/system_env/bin/python3 $< $(ARGS)
pyperf_%_pypy: %.py $(PYPY_BENCH_ENV) pyston/build/system_env/bin/python
	$(PYPERF) pyston/build/pypy_env/bin/python3 $< $(ARGS)

dbg_%: dbg_%_dbg


# UNSAFE build target
# If you're making changes to cpython but don't want to rebuild the traces or anything beyond the python binary,
# you can call this target for a much faster build.
# Skips cpython_bc_build, aot_gen, and cpython_unopt_install
# If you want to run the skipped steps, you will have to touch a file before doing a (safe) rebuild
unsafe_dbg:
	$(MAKE) -C pyston/build/cpython_dbg_build
	/bin/cp pyston/build/cpython_dbg_build/pyston pyston/build/cpython_dbg_install/usr/bin/python$(PYTHON_MAJOR).$(PYTHON_MINOR)
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

.PRECIOUS: pyston/test/external/test_%.optoutput
$(call make_test_build,optoutput,pyston/build/opt_env/bin/python)

$(call make_test_build,dbgexpected,pyston/build/stockdbg_env/bin/python)
.PRECIOUS: pyston/test/external/test_%.dbgexpected pyston/test/external/test_%.dbgoutput
$(call make_test_build,dbgoutput,pyston/build/dbg_env/bin/python)

test_%: pyston/test/external/test_%.expected pyston/test/external/test_%.output
	pyston/build/system_env/bin/python pyston/test/external/helpers.py compare $< $(patsubst %.expected,%.output,$<)

testopt_%: pyston/test/external/test_%.expected pyston/test/external/test_%.optoutput
	pyston/build/system_env/bin/python pyston/test/external/helpers.py compare $< $(patsubst %.expected,%.output,$<)

testdbg_%: pyston/test/external/test_%.dbgexpected pyston/test/external/test_%.dbgoutput
	pyston/build/system_env/bin/python pyston/test/external/helpers.py compare $< $(patsubst %.dbgexpected,%.dbgoutput,$<)

cpython_testsuite: pyston/build/cpython_unopt_build/pyston
	OPENSSL_CONF=$(abspath pyston/test/openssl_dev.cnf) $(MAKE) -C pyston/build/cpython_unopt_build test

cpython_testsuite_opt: pyston/build/cpython_opt_build/pyston
	OPENSSL_CONF=$(abspath pyston/test/openssl_dev.cnf) $(MAKE) -C pyston/build/cpython_opt_build test

cpython_testsuite_dbg: pyston/build/cpython_dbg_build/pyston
	OPENSSL_CONF=$(abspath pyston/test/openssl_dev.cnf) $(MAKE) -C pyston/build/cpython_dbg_build test

# Note: cpython_testsuite is itself parallel so we might need to run it not in parallel
# with the other tests
_runtests: pyston/test/caches_unopt pyston/test/test_rebuild_packages_unopt pyston/test/deferred_stack_decref_unopt pyston/test/getattr_caches_unopt test_django test_urllib3 test_setuptools test_six test_requests cpython_testsuite
_runtestsdbg: pyston/test/caches_dbg pyston/test/test_rebuild_packages_dbg testdbg_django testdbg_urllib3 testdbg_setuptools testdbg_six testdbg_requests cpython_testsuite_dbg
_runtestsopt: pyston/test/caches_opt pyston/test/test_rebuild_packages_opt testopt_django testopt_urllib3 testopt_setuptools testopt_six testopt_requests cpython_testsuite_opt

test: pyston/build/system_env/bin/python pyston/build/unopt_env/bin/python
	rm -f $(wildcard pyston/test/external/*.output)
	# Test mostly tests the CAPI so don't rerun it with different JIT_MIN_RUNS
	# Note: test_numpy is internally parallel so we might have to re-separate it
	# into a separate step
	$(MAKE) _runtests test_numpy
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

# llvm-bolt must be build outside of dpkg-buildpackage or it will segfault
.PHONY: package
package: bolt
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

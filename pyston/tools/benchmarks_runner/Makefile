# Usage:
# In nitrous/ run:
# 	make build/opt_env/update.stamp build/system_env/update.stamp build/pypy_env/update.stamp
# Then here run
# 	make analyze

.DEFAULT_GOAL:=analyze

.NOTPARALLEL:

results:
	mkdir -p $@

pypy-benchmarks: 
	hg clone https://foss.heptapod.net/pypy/benchmarks $@

BUILDS:=system systempyston

ENV_DIR:=../../../build/

# To run the pypy benchmarks on 3.8, you need to update
# pypy-benchmarks/unladen_swallow/perf.py, and comment out lines
# 1600 and 1601 which say:
#    if len(errors) > 0:
#        raise errors[-1]
# PYPY_BENCHS=ai,bm_chameleon,bm_dulwich_log,bm_mako,bm_mdp,chaos,cpython_doc,crypto_pyaes,deltablue,django,eparse,fannkuch,float,genshi_text,genshi_xml,go,hexiom2,html5lib,json_bench,meteor-contest,nbody_modified,nqueens,pickle,pickle_dict,pickle_list,pidigits,pyflate-fast,pyxl_bench,raytrace-simple,richards,scimark_fft,scimark_lu,scimark_montecarlo,scimark_sor,scimark_sparsematmult,spectral-norm,spitfire2,spitfire2,spitfire_cstringio2,sqlalchemy_declarative,sqlalchemy_imperative,sqlitesynth,sympy_expand,sympy_integrate,sympy_str,sympy_sum,telco,twisted_iteration,twisted_names,twisted_pb,twisted_tcp,unpickle,unpickle_list
PYPY_BENCHS=ai,bm_chameleon,bm_dulwich_log,bm_mako,bm_mdp,chaos,cpython_doc,crypto_pyaes,deltablue,eparse,fannkuch,float,go,hexiom2,html5lib,json_bench,meteor-contest,nbody_modified,nqueens,pickle,pickle_dict,pickle_list,pidigits,pyflate-fast,pyxl_bench,raytrace-simple,richards,scimark_fft,scimark_lu,scimark_montecarlo,scimark_sor,scimark_sparsematmult,spectral-norm,sqlalchemy_declarative,sqlalchemy_imperative,sqlitesynth,sympy_expand,sympy_integrate,sympy_str,sympy_sum,twisted_iteration,twisted_names,twisted_pb,twisted_tcp,unpickle,unpickle_list
results/pypybench-%.json: | $(ENV_DIR)%_env/bin/python pypy-benchmarks results
	PYTHONPATH=..:$(PYTHONPATH) python3 -c "import tune; tune.tune()"
	/usr/bin/time --verbose --output=$(patsubst %.json,%.time,$@) $(ENV_DIR)system_env/bin/python3 pypy-benchmarks/runner.py --changed=$(firstword $|) -o $@ --benchmarks=$(PYPY_BENCHS)
	PYTHONPATH=..:$(PYTHONPATH) python3 -c "import tune; tune.untune()"
pypybench_results: $(patsubst %,results/pypybench-%.json,$(BUILDS))

# Usage: make_benchmark,NAME,NITERS
define make_benchmark
$(eval
results/$(1)-%.json: ../../macrobenchmarks/benchmarks/bm_$(1)/run_benchmark.py | $(ENV_DIR)%_env/bin/python results
	PYTHONPATH=..:$(PYTHONPATH) python3 -c "import tune; tune.tune()"
	/usr/bin/time --verbose --output=$$(patsubst %.json,%.time_,$$@) $$(firstword $$|) $$< --legacy $(2) $$@ || echo failed > $$@
	PYTHONPATH=..:$(PYTHONPATH) python3 -c "import tune; tune.untune()"
	mv $$(patsubst %.json,%.time_,$$@) $$(patsubst %.json,%.time,$$@)
	cat $$(patsubst %.json,%.time,$$@)

$(1)_results: $(patsubst %,results/$(1)-%.json,$(BUILDS))
all_results: $(1)_results
)
endef

$(call make_benchmark,flaskblogging,80000)
$(call make_benchmark,pylint,200)
$(call make_benchmark,djangocms,10000)
$(call make_benchmark,mypy,100)
$(call make_benchmark,pycparser,50)
$(call make_benchmark,pytorch_alexnet_inference,1000)
$(call make_benchmark,gunicorn,10000)
$(call make_benchmark,aiohttp,10000)
$(call make_benchmark,json,1000)
$(call make_benchmark,thrift,10000)
$(call make_benchmark,kinto,30000)

.PHONY: analyze
analyze: all_results # pypybench_results
	python3 analyze.py

quick_analyze: results/djangocms-system.json results/djangocms-opt.json results/flaskblogging-system.json results/flaskblogging-opt.json results/kinto-system.json results/kinto-opt.json
	python3 analyze.py

quick_analyze_systempyston: results/djangocms-system.json results/djangocms-systempyston.json results/flaskblogging-system.json results/flaskblogging-systempyston.json results/kinto-system.json results/kinto-systempyston.json
	python3 analyze.py

clean:
	rm -rfv results

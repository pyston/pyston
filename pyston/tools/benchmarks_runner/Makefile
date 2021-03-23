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

BUILDS:=opt system pypy

ENV_DIR:=../../build/

# To run the pypy benchmarks on 3.8, you need to update
# pypy-benchmarks/unladen_swallow/perf.py, and comment out lines
# 1600 and 1601 which say:
#    if len(errors) > 0:
#        raise errors[-1]
results/pypybench-%.json: | $(ENV_DIR)%_env/bin/python pypy-benchmarks results
	PYTHONPATH=..:$(PYTHONPATH) python3 -c "import tune; tune.tune()"
	/usr/bin/time --verbose --output=$(patsubst %.json,%.time,$@) $(ENV_DIR)system_env/bin/python3 pypy-benchmarks/runner.py --changed=$(firstword $|) -o $@
	PYTHONPATH=..:$(PYTHONPATH) python3 -c "import tune; tune.untune()"
pypybench_results: $(patsubst %,results/pypybench-%.json,$(BUILDS))

# Usage: make_benchmark,NAME,NITERS
define make_benchmark
$(eval
results/$(1)-%.json: ../../macrobenchmarks/benchmarks/$(1).py | $(ENV_DIR)%_env/bin/python results
	PYTHONPATH=..:$(PYTHONPATH) python3 -c "import tune; tune.tune()"
	/usr/bin/time --verbose --output=$$(patsubst %.json,%.time_,$$@) $$(firstword $$|) $$< $(2) $$@ || echo failed > $$@
	PYTHONPATH=..:$(PYTHONPATH) python3 -c "import tune; tune.untune()"
	mv $$(patsubst %.json,%.time_,$$@) $$(patsubst %.json,%.time,$$@)
	cat $$(patsubst %.json,%.time,$$@)

$(1)_results: $(patsubst %,results/$(1)-%.json,$(BUILDS))
all_results: $(1)_results
)
endef

$(call make_benchmark,flaskblogging,80000)
$(call make_benchmark,pylint_bench,200)
$(call make_benchmark,djangocms,10000)
$(call make_benchmark,mypy_bench,100)
$(call make_benchmark,pycparser_bench,50)
$(call make_benchmark,pytorch_alexnet_inference,1000)
$(call make_benchmark,gunicorn,10000)
$(call make_benchmark,aiohttp,10000)
$(call make_benchmark,json_bench,1000)
$(call make_benchmark,thrift_bench,10000)

.PHONY: analyze
analyze: all_results # pypybench_results
	python3 analyze.py

quick_analyze: results/djangocms-system.json results/djangocms-opt.json results/flaskblogging-system.json results/flaskblogging-opt.json
	python3 analyze.py

clean:
	rm -rfv results

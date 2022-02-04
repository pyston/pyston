import json
import numpy as np
import os
import re
import subprocess
import sys

join = os.path.join
def rel(path):
    return join(os.path.dirname(__file__), path)

builds = []

def getQuantiles(times):
    iteration_times = times[1:] - times[:-1]

    r = {}
    r['mean'] = np.mean(iteration_times)
    r['p99'] = np.quantile(iteration_times, 0.99)

    return r

class FailedException(Exception):
    pass

RESULTS_DIR = rel("results")

def loadTimings(bench, build):
    data = open(join(RESULTS_DIR, "%s-%s.json" % (bench, build))).read()
    if data == "failed\n":
        raise FailedException()
    return np.array(json.loads(data))

def loadMemoryUsage(bench, build):
    data = open(join(RESULTS_DIR, "%s-%s.time" % (bench, build))).read()
    m = re.search(r"Maximum resident set size \(kbytes\): (\d+)", data)
    assert m, data
    return 0.001 * int(m.group(1))

def determineWarmup(timings, window):
    final_lookback = int(len(timings) * 0.25)
    final_latency = (timings[-1] - timings[-1 - final_lookback]) / final_lookback

    avg = (timings[window:] - timings[:-window]) / window

    return np.where(avg < 1.05 * final_latency)[0][0] + int(window / 2)

def calculateWindow(timings):
    return max(1, int(len(timings) / 100))

def analyze(bench, normalized=False, p99=False):
    if p99:
        print("Build warmup mean p99 RSS")
    else:
        print("Build warmup mean RSS")

    try:
        baseline = getQuantiles(loadTimings(bench, "system"))
    except FailedException:
        pass

    all_quantiles = {}

    for build in builds:
        print(build, end=' ')
        try:
            timings = loadTimings(bench, build)
        except FailedException:
            print("Failed")
            continue
        except FileNotFoundError:
            print("na")
            continue

        window = calculateWindow(timings)
        warmup_done_idx = determineWarmup(timings, window)

        if warmup_done_idx > max(2, window):
            # print("Took %d iterations (%.1fs) to warm up" % (warmup_done_idx, timings[warmup_done_idx] - timings[0]))
            print("%.1fs" % (timings[warmup_done_idx] - timings[0]), end=" ")
            if warmup_done_idx > 0.75 * len(timings):
                raise Exception("Need to run the %s benchmark longer to let %s warm up" % (bench, build))

            # print("during warmup", getQuantiles(timings[:warmup_done_idx]))
            # print("post-warmup", getQuantiles(timings[warmup_done_idx:]))
            quantiles = getQuantiles(timings[warmup_done_idx:])
        else:
            print("N/A", end=" ")
            quantiles = getQuantiles(timings)
        all_quantiles[build] = quantiles

        print("%.1fms (%.2fx)" % (1000.0 * quantiles['mean'], baseline['mean'] / quantiles['mean']), end=" ")
        if p99:
            print("%.2fms (%.2fx)" % (1000.0 * quantiles['p99'], baseline['p99'] / quantiles['p99']), end=" ")

        mem = loadMemoryUsage(bench, build)
        print("%.1fMB" % mem, end=' ')
        quantiles['maxrss'] = mem

        print()
    return all_quantiles

def plot(bench):
    import matplotlib.pyplot as plt

    for build in builds:
        timings = loadTimings(bench, build)
        window = calculateWindow(timings)
        avg = (timings[window:] - timings[:-window]) / window
        plt.plot(timings[window:] - timings[int((window + 1) / 2)], 1.0 / avg)
    plt.legend(builds)
    plt.show()

def plotLatencyHistogram(bench, build):
    import matplotlib.pyplot as plt

    timings = loadTimings(bench, build)
    latencies = timings[1:] - timings[:-1]
    range = (0, np.quantile(latencies, 0.999))
    p99 = np.quantile(latencies, 0.99)
    # plt.yscale("log")
    plt.hist(latencies, bins=40, range=range, histtype="stepfilled")
    plt.plot((p99, p99), (0, 0.02 * len(latencies)))
    plt.title("%s latency histogram on %s" % (build, bench))
    plt.show()

def plotLatencies(bench, builds, n=5000):
    import matplotlib.pyplot as plt

    for build in builds:
        timings = loadTimings(bench, build)
        latencies = timings[1:] - timings[:-1]
        plt.plot(latencies[-n:])
    plt.legend(builds)
    plt.ylim(0, plt.ylim()[1])
    plt.xlim(0, 500)
    plt.show()

if __name__ == "__main__":
    # benches = ["flaskblogging", "pylint_bench", "djangocms", "mypy_bench", "pycparser_bench", "pytorch_alexnet_inference"]
    benches = []
    for fn in os.listdir(os.path.join(os.path.dirname(__file__), "results")):
        if fn.endswith("-system.json"):
            benches.append(fn.rsplit('-', 1)[0])
        if fn.endswith(".json"):
            builds.append(fn.split('-')[-1][:-5])
    builds = list(set(builds))

    if 0:
        for bench in ("flaskblogging", "djangocms"):
            plot(bench)
            for build in builds:
                plotLatencyHistogram(bench, build)
        sys.exit()

    if 0:
        plotLatencies("djangocms", ["system", "opt", "pypy"])
        sys.exit()


    for build in builds:
        print(build)
        try:
            subprocess.check_call([os.path.join(os.path.dirname(__file__), "../../build/%s_env/bin/python" % build), "-c", "import sys; print(sys.version)"])
        except FileNotFoundError:
            print("not found")
        print()

    all_quantiles = {}
    for bench in benches:
        print(bench)
        normalized = bench in ("pylint_bench", "mypy_bench", "pycparser_bench")
        p99 = bench in ("flaskblogging", "djangocms", "kinto")
        all_quantiles[bench] = analyze(bench, normalized=normalized, p99=p99)
        print()
    # plotLatencyHistogram("flaskblogging", "pypy")

    averaged_benches = ["flaskblogging", "djangocms"]
    print("Geomean of %s:" % ("+".join(averaged_benches)))
    for build in builds:
        print("%s:" % build, end=" ")
        for type in "mean", "p99", "maxrss":
            t = 1.0
            for bench in averaged_benches:
                if bench not in all_quantiles or build not in all_quantiles[bench]:
                    t *= float('nan')
                else:
                    t *= all_quantiles[bench]['system'][type] / all_quantiles[bench][build][type]
            t **= (1.0 / len(averaged_benches))
            if type == "maxrss":
                t = 1.0 / t
                style = "worse"
            else:
                style = "better"
            print("%s:%.2fx %s" % (type, t, style), end=" ")
        print()

    print()
    for build in builds:
        fn = join(RESULTS_DIR, "pypybench-%s.json" % build)
        if not os.path.exists(fn):
            continue

        data = json.load(open(fn))
        t = 1.0
        names = []
        for name, type, this_data in data['results']:
            if type == "ComparisonResult":
                t *= this_data['avg_base'] / this_data['avg_changed']
            elif type == "RawResult":
                t *= sum(this_data['base_times']) / sum(this_data['changed_times'])
            else:
                assert type == "SimpleComparisonResult", type
                if this_data['changed_time'] == -1:
                    # print(build, "failed", name)
                    continue
                t *= this_data['base_time'] / this_data['changed_time']
            names.append(name)
            assert t > 0, this_data
        assert len(set(names)) == len(names)
        t **= (1.0 / len(names))
        print(build, "pypybench (%d benches hash %d): %.2fx" % (len(names), hash(tuple(names)), t))

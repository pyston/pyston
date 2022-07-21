"""
Testing script, for identifying opportunities to make pyston-lite faster

Usage:
    make perf_report
    make perf_report_baseline
    python3 compare.py

Will try to identify the largest differences between the two perf reports
"""

from collections import defaultdict
import subprocess

def parse(fn):
    r = defaultdict(int)
    total = None
    for l in subprocess.check_output(["perf", "report", "-n", "--no-children", "-g", "none", "-i", fn]).decode("utf8").split('\n'):
        if l.startswith("#"):
            if "Event count" in l:
                total = int(l.split()[-1])
            continue
        if '%' not in l:
            continue

        l = l.split()
        if '[k]' in l:
            l[-1] = "kernel"
        # r[l[-1]] = int(l[1])
        pct = float(l[0].rstrip("%"))
        if pct == 0.0:
            pct = 0.0025
        r[l[-1]] = pct * total * 0.01
    return r

def compare():
    baseline = defaultdict(int)
    lite = defaultdict(int)

    baseline = parse("/tmp/perf_baseline.data")
    lite = parse("/tmp/perf_lite.data")

    baseline_total = sum(baseline.values())
    lite_total = sum(lite.values())

    print("%.1fB\t%.1fB\t" % (baseline_total * 1e-9, lite_total * 1e-9), "%+.2f%%" % ((lite_total - baseline_total) / baseline_total * 100.0))
    print("="*40)

    for d in (lite, baseline):
        # for k, v in list(d.items()):
            # if "EvalFrame" in k:
                # d["_EvalFrame"] += d.pop(k)
                # d["_EvalFrame"] += d.pop(k)
        d["call_functionFunction"] += d.pop("call_functionFunction2", 0)
        d["call_functionFunction"] += d.pop("frame_dealloc", 0)

        d["fib.py:1:fib"] += d.pop("fib.py:2:fib", 0)

        d["cmp_outcomePyCmp_LELongLong2"] += d.pop("PyObject_RichCompare", 0)
        d["cmp_outcomePyCmp_LELongLong2"] += d.pop("long_richcompare", 0)
        d["cmp_outcomePyCmp_LELongLong2"] += d.pop("cmp_outcomePyCmp_LE", 0)

        d["PyNumber_SubtractLongLong2"] += d.pop("PyNumber_Subtract", 0)
        d["PyNumber_SubtractLongLong2"] += d.pop("long_sub", 0)

        d["PyNumber_AddLongLong2"] += d.pop("PyNumber_Add", 0)
        d["PyNumber_AddLongLong2"] += d.pop("long_add", 0)

        d["_PyEval_EvalFrameDefault"] += d.pop("_PyEval_EvalFrame_AOT_Interpreter", 0)
        d["_PyEval_EvalFrameDefault"] += d.pop("_PyEval_EvalFrame_AOT", 0)

    all = set(baseline)
    all.update(lite)

    diffs = []
    for k in all:
        diffs.append((abs(baseline.setdefault(k, 0) - lite.setdefault(k, 0)), k))

    diffs.sort(reverse=True)
    for t in diffs[:10]:
        k = t[-1]
        f = baseline[k]
        l = lite[k]
        # print("%4.1f%%\t% 4.1f%%\t" % (100.0 * f / baseline_total, 100.0 * l / lite_total), "%+.2f%%" % ((l - f) / baseline_total * 100.0), k)
        print("%d\t%d\t" % (f * 1e-6, l * 1e-6), "%+.2f%%" % ((l - f) / baseline_total * 100.0), k)

def showPercents():
    baseline = parse("/tmp/perf_baseline.data")
    lite = parse("/tmp/perf_lite.data")
    baseline_total = sum(baseline.values())
    lite_total = sum(lite.values())

    l = list(lite.items())
    l.sort(key=lambda p:-p[1])

    for k, v in l[:20]:
        print("%4.1f%%\t%s" % (100.0 * v / lite_total, k))

if __name__ == "__main__":
    # showPercents()
    compare()

#!python3
"""
A script to generate profiling flamegraphs.

To see generally where the program is spending its time, just do
    python3 tools/perf_flamegraph.py
The bottom-most bars show what functions the program was in when the
samples were taken, with their call sources stacked above them. (This
is the reverse of the flamegraph default.)

To identify what causes calls to the PyArg_* functions, one can do
    python3 tools/perf_flamegraph.py --function PyArg
which will generate a flamegraph showing the callers of these functions, indicating
callsites that could be migrated to faster functions.

To see where we spend our time, one can do
    python3 tools/perf_flamegraph.py --function EvalFrame --reverse
to see what code EvalFrame ends up calling into

Currently this script requires an Intel CPU, since it uses the LBR to get call
traces.  It may be possible to use "--call-graph dwarf" instead, though when I tried that
once it didn't immediately work.

This script is similar to the earlier call_sources.py, but uses perf to
generate the samples instead of gdb.  This makes it much lower overhead, and it
measures execution time instead of execution counts.
The downside is it only works well for functions that are called fairly often,
since it doesn't selectively sample for the specific function you ask for.
"""


import argparse
import collections
import os
import re
import signal
import subprocess
import sys
import time

assert sys.version_info >= (3,)

from call_sources import STOP_AT

def relpath(path):
    return os.path.join(os.path.dirname(__file__), "..", path)

if __name__ == "__main__":
    parser = argparse.ArgumentParser("aot_gen")
    parser.add_argument("--binary", default=relpath("build/unopt_env/bin/python"), help="The binary to test")
    parser.add_argument("--target", default=relpath("python/benchmarks/run_benchmarks.py"), help="The script to run. can include arguments separated by spaces")
    parser.add_argument("--function", default=[], action="append", help="The set of functions to evaluate. If empty, all samples are included; if non-empty, samples are only included if some function in the stack trace contains one of the specified functions (case-sensitive)")
    parser.add_argument("--from-cache", action="store_true", help="Whether to reuse the perf results from the previous run.  If not specified, this script will run the benchmarking target.  If given, the previous results will be reused.  This is useful for investigating multiple flamegraphs from the same data")
    parser.add_argument("--svg-viewer", default="google-chrome", help="SVG viewer. google-chrome is the only program I've found that supports interactive svgs")
    parser.add_argument("--delay", default="0", help="ms to wait before starting recording")
    parser.add_argument("--preserve-fraction", action="store_true", help="Add filtered samples as empty samples to preserve fractions")
    parser.add_argument("--reverse", action="store_true", help="Reverse stack order. By default this script produces flame graphs with the lowest stack frame at the bottom (reverse of typical flamegraphs), which is useful for identifying callers of a specific function.  If --reverse is passed, the lower stack frames are higher on the flamegraph (typical flamegraph usage), which helps identify what contributes to the runtime of a specific function")
    parser.add_argument("--stop-at", action="append", help="Provide additional 'stop-at' functions.  Our stack traces are quite deep and it is generally unhelpful to include all of them.  This script contains some heuristics on where to cut a stack trace, and if you provide more functions here it will cut the stack trace at that function")
    parser.add_argument("--width", default=2000, type=int, help="Width of the generated svg image")

    args = parser.parse_args()

    if args.stop_at:
        STOP_AT += [s.encode("ascii") for s in args.stop_at]

    if not args.from_cache:
        print(args.binary, args.target)
        subprocess.check_call(["perf", "record", "--call-graph", "lbr", "--delay", args.delay, args.binary] + args.target.split())

        p1 = subprocess.Popen(["perf", "script"], stdout=subprocess.PIPE)
        p2 = subprocess.Popen([relpath("tools/FlameGraph/stackcollapse-perf.pl")], stdin=p1.stdout, stdout=open("perf_collapsed.txt", "w"))
        assert p1.wait() == 0
        assert p2.wait() == 0

    total_count = 0
    included_count = 0
    to_write = []
    for l in open("perf_collapsed.txt"):
        stack, count = l.split()
        total_count += int(count)
        stack = stack.split(';')
        end_at = None
        for i, f in reversed(list(enumerate(stack))):
            if not args.reverse and end_at is None and any(s in f for s in args.function):
                end_at = i + 1
            if f.encode("ascii") in STOP_AT or (args.reverse and any(s in f for s in args.function)):
                l = "%s %s\n" % (';'.join(stack[i:end_at]), count)
                break
        else:
            l = "%s %s\n" % (';'.join(stack[:end_at]), count)

        # if args.function and stack[-1].decode("ascii") != args.function:
        if args.function and not any(f in l for f in args.function):
            continue

        included_count += int(count)
        to_write.append(l)
    if args.preserve_fraction and total_count > included_count:
        to_write.append(" %d\n" % (total_count - included_count))
    to_write = ''.join(to_write)

    flamegraph_args = []
    if not args.reverse:
        # Our reverse is opposite of flamegraph reverse
        flamegraph_args.append("--reverse")
    if args.function:
        flamegraph_args += ["--title", " ".join(args.function)]
    p3 = subprocess.Popen([relpath("tools/FlameGraph/flamegraph.pl"), "--minwidth", "1", "--width", str(args.width), "--fontsize", "8", "--subtitle", "%d (%.1f%%) of %d total samples" % (included_count, 100.0 * included_count / total_count, total_count)] + flamegraph_args, stdin=subprocess.PIPE, stdout=open("flamegraph.svg", "w"))
    p3.communicate(to_write.encode("ascii"))

    print("Filtered to %.2f%% of total samples" % (100.0 * included_count / total_count))

    assert p3.wait() == 0

    subprocess.check_call([args.svg_viewer, "flamegraph.svg"])

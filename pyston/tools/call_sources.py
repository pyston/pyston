#!python3
"""
A script to show the sources of a certain function being called

Runs the program with gdb, collects backtraces when the function
is called, and counts and sorts the occurrences.
"""

from __future__ import print_function

import argparse
import collections
import os
import re
import signal
import subprocess
import sys
import time

assert sys.version_info >= (3,)

StackFrame = collections.namedtuple("StackFrame", ("function", "filename", "line", "source"))

STOP_AT = [
    b"_PyEval_EvalFrame_AOT",
    b"_PyEval_EvalFrameDefault",
    b"PyType_Ready",
    b"builtin_getattr",
    b"builtin_isinstance",
    b"builtin_dir",
    b"builtin_any",
    b"slot_tp_getattr_hook",
    b"slot_sq_contains",
    b"slot_tp_init",
    b"slot_mp_subscript",
    b"slot_mp_ass_subscript",
    b"slot_tp_richcompare",
    b"slot_nb_or",
    b"slot_nb_bool",
    b"slot_tp_call",
    b"slot_tp_iter",
    b"_PyFrame_New_NoTrack",
    b"PyObject_Str",
    # b"module_getattro",
    # b"type_getattro",
    b"super_getattro",
    # b"builtin___build_class__",
]

class CStackController:
    def gdbGetTraceCommand(self):
        return b"bt 30"

    @staticmethod
    def _truncate_trace(trace):
        done = False
        for i, l in enumerate(trace):
            if done and l.startswith(b'#'):
                return trace[:i]
            for s in STOP_AT:
                if s in l:
                    done = True
                    break
        return trace

    @staticmethod
    def _canonicalize_trace(trace):
        r = []
        funcname = None
        for l in trace:
            if l.startswith(b'(gdb) '):
                l = l[6:]
            if l.startswith(b'#'):
                s = l.split(b'(')[0].split()[-1]
                funcname = s
            if b'    at ' in l or b') at ' in l:
                s = l.rsplit(b' at ')[-1]
                # r.append(funcname + b' at ' + s)
                s = re.sub(b"([/a-zA-Z0-9])+/cpython_bc_build/", b"", s)
                assert funcname, (l, trace)
                r.append(funcname + b'\n')
                r.append(b'    at ' + s)
                funcname = None
        return r

        r = []
        for l in trace:
            if l.startswith(b"    at"):
                r.append(l)
            else:
                l = re.sub(b"0x([0-9a-f])+", b"XXX", l)
                l = re.sub(b"=-?([0-9])+", b"=XXX", l)
                l = re.sub(b"<([a-zA-Z _])+>", b"<XXX>", l)
                l = re.sub(b'"([a-zA-Z _])+"', b'"XXX"', l)
                r.append(l)

        r2 = []
        for l in r:
            if l.startswith(b"(gdb)") or l.startswith(b"#"):
                r2.append(l)
            else:
                s = r2[-1][:-1]
                if not s.endswith(b' '):
                    s += b' '
                r2[-1] = s + l.lstrip()

        r3 = []
        for l in r2:
            s1, s2 = l.rsplit(b' at ', 1)
            r3.append(s1 + b'\n')
            r3.append(b'    at ' + s2)

        return r3

    def processTrace(self, trace):
        trace = CStackController._truncate_trace(trace)
        trace = CStackController._canonicalize_trace(trace)

        r = []
        for i in range(0, len(trace), 2):
            func = trace[i].strip()
            loc = trace[i+1].strip().split()[-1]
            filename, line = loc.split(b":")
            r.append(StackFrame(func, filename, int(line), None))
        return tuple(r)

class PyStackController:
    def __init__(self, limit=1):
        self.limit = limit
        self.cmd = b"""
dis 1
set $d = PyDict_New()
set $Py_file_input = 257
set $ret = PyRun_String("import traceback, sys; traceback.print_stack(file=sys.stdout, limit=%d); sys.stdout.flush()", $Py_file_input, $d, $d)
set $_unused_void = Py_DecRef($d)
set $_unused_void = Py_DecRef($ret)
set $_unused_void = PyErr_Clear()
en 1
        """.strip() % (max(limit, 15) + 1) # empty lines mean "repeat last command", limit+1 to include the Run_String frame

    def gdbGetTraceCommand(self):
        return self.cmd

    def processTrace(self, trace):
        trace = trace[:-1] # "<string>" from our traceback code
        trace[0] = trace[0].replace(b'(gdb) ', b'')

        r = []
        for i in range(-2, -2 * self.limit - 1, -2):
            _file, filename, rest = trace[i].split(b'"')
            _comma, _line, line, _in, function = rest.split()
            line = int(line[:-1])
            source = trace[i+1].strip()
            r.append(StackFrame(function, filename, line, source))
        return tuple(r)

def collectTraces(args, controller):
    """
    skip: specifies how much warmup to skip
          int: that number of hits to the target function
          float: that many seconds of warmup
          bytes: until this string appears in the output
    """

    cmdline = [args.binary, "-u"] + args.target.split()

    skip = args.skip
    try:
        skip = int(skip)
    except ValueError:
        try:
            skip = float(skip)
        except ValueError:
            skip = skip.encode("ascii")

    function = args.function.encode("ascii")

    p = subprocess.Popen(["gdb", "--args"] + cmdline, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    try:

        def send(s):
            p.stdin.write(s + b'\n')

        marker = [123450000]
        def read_results():
            m = marker[0]
            marker[0] += 1
            send(b'print %d' % m)
            p.stdin.flush()

            r = []
            while True:
                l = p.stdout.readline()
                if b'= %d' % m in l:
                    break
                r.append(l)
            return r

        if isinstance(skip, int):
            send(b"break " + function)
            send(b"run")
            assert skip > 0, "sorry we always skip the first hit"
            skip -= 1
            skipped = 0
            while skipped < skip:
                toskip = min(1000, skip - skipped)
                send(b"c %d" % toskip)
                read_results()
                skipped += toskip
                print("skipped", skipped)
        elif isinstance(skip, float):
            send(b"run")
            p.stdin.flush()
            time.sleep(skip)
            p.send_signal(signal.SIGINT)
            send(b"break " + function)
            # print(b''.join(read_results()).decode("ascii"))
        elif isinstance(skip, bytes):
            send(b"run")
            p.stdin.flush()
            while True:
                l = p.stdout.readline()
                print(l)
                if skip in l:
                    break
            p.send_signal(signal.SIGINT)
            send(b"break " + function)
        else:
            assert 0, type(skip)

        send(b"c")
        read_results()
        # print(b''.join(read_results()).decode("ascii"))

        traces = []
        for i in range(args.n):
            if i % 100 == 0:
                print("collected", i)
            send(controller.gdbGetTraceCommand())
            traces.append(read_results())
            send(b"c %d" % args.every)
            read_results()

        send(b"set confirm off")
        send(b"quit")
        p.stdin.flush()
        assert p.wait(1) == 0
    finally:
        p.terminate()

    return traces

def formatForPrint(stack_frame):
    s = stack_frame.function + b'\n'
    if stack_frame.source:
        s += b"    " + stack_frame.source + b'\n'
    s += b"    %s:%d" % (stack_frame.filename, stack_frame.line)
    return s.decode("ascii")

def removeDuplicates(stack_list):
    r = []
    func = None
    for s in stack_list:
        if s.function == func:
            continue
        r.append(s)
        func = s.function
    return tuple(r)

if __name__ == "__main__":
    parser = argparse.ArgumentParser("aot_gen")
    parser.add_argument("--binary", default="build/unopt_env/bin/python")
    parser.add_argument("--target", default="python/benchmarks/django_template_verbose.py 1000000")
    parser.add_argument("function")
    parser.add_argument("--skip", default="iteration 20")
    parser.add_argument("-n", default=1000, type=int)
    parser.add_argument("--every", default=107, type=int)
    parser.add_argument("--show", default=20, type=int)
    parser.add_argument("--flame-graph", action="store_true")
    parser.add_argument("--pystack", action="store_true")
    parser.add_argument("--pydepth", type=int, default=1)
    # not super well-motivated, but I'm seeing some stack frames with weird recursive frames (there's no recursion in the function)
    # Maybe useful to handle actually-recursive functions too?
    parser.add_argument("--no-remove-duplicates", action="store_false", dest="remove_duplicates", default=True)
    parser.add_argument("--from-cache", action="store_true")

    args = parser.parse_args()

    if args.pystack:
        controller = PyStackController(args.pydepth)
    else:
        controller = CStackController()

    import pickle
    if args.from_cache:
        traces = pickle.load(open("traces.pkl", "rb"))
    else:
        traces = collectTraces(args, controller)
        pickle.dump(traces, open("traces.pkl", "wb"))
    print("analyzing", len(traces), "traces", file=sys.stderr)

    counts = collections.defaultdict(int)
    for t in traces:
        s = controller.processTrace(t)
        if args.remove_duplicates:
            s = removeDuplicates(s)
        counts[s] += 1

    counts = sorted(counts.items(), key=lambda p: -p[1])
    if args.flame_graph:
        with open("/tmp/out.perf", "w") as f:
            for k, v in counts:
                print(';'.join([l.function.decode("ascii") for l in k]), v, file=f)
        subprocess.check_call([os.path.join(os.path.dirname(__file__), "FlameGraph/flamegraph.pl"), "/tmp/out.perf", "--width", "3000", "--minwidth", "45"], stdout=open("/tmp/out.svg", "w"))
        subprocess.check_call(["xdg-open", "/tmp/out.svg"])
    else:
        for k, v in counts[:args.show]:
            print("\n%.1f%%" % (100.0 * v / args.n))
            for l in k:
                print(formatForPrint(l))

import base64
import difflib
import hashlib
import os
import sys
import subprocess
import re
import time

def parse_output(output):
    result = []
    for l in output.split('\n'):
        # nosetest
        m = re.match("Ran (\\d+) tests in", l)
        if m:
            result.append({"ran": int(m.group(1))})
            continue
        for res_type in ("errors", "failures", "skipped", "expected failures"):
            m = re.match("(FAILED|OK) \\(.*%s=(\\d+).*\\)" % res_type, l)
            if m:
                result[-1][res_type] = int(m.group(2))

        # py.test
        m = re.match(".* in \\d+[.]\\d+ seconds [=]*$", l)
        if not m:
            # newer pytest?
            m = re.match(".*passed.* in \\d+[.]\\d+s", l)
        if m:
            result.append({})
            matches = re.findall("\\d+ \\w+", l)
            for m in matches:
                num, kind = m.split()
                if kind == "seconds":
                    continue
                result[-1][kind] = int(num)
    return result

def _formatHash(hash):
    s = ""
    s += "expected_log_hash = '''\n"
    for i in range(0, len(hash), 60):
        s += hash[i:i+60] + '\n'
    s += "'''"
    return s

def get_test_results(cmd, cwd, env=None, show_output=True):
    print("Running", cmd)
    start = time.time()
    process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=open("/dev/null"), cwd=cwd, env=env)
    output = []
    for l in process.stdout:
        l = l.decode("utf8")
        if show_output:
            print(l[:-1])
        output.append(l)
    s = process.stdout.read()
    assert not s, s
    output = ''.join(output)
    errcode = process.wait()
    print("Took %.1fs to run the test" % (time.time() - start))

    print()
    print("Return code:", errcode)
    assert errcode in (0, 1), "\n\n%s\nTest process crashed (error code %d)" % (output, errcode)

    return output

def run_test(cmd, cwd, expected_output, env=None, show_output=True):
    output = get_test_results(cmd, cwd, env=env, show_output=show_output)
    return compare(expected_output, output, show_output=not show_output)

def compare(expected_output, output, show_output=True):
    expected_log = process_log(expected_output)
    expected = parse_output(expected_output)

    do_log_check = True
    allowed_differences = {}
    on_failure_messages = []

    directives = []
    for l in expected_log:
        if not l.startswith("PYSTONTEST: "):
            continue

        l = l.split(" ")
        directive = l[1]
        args = l[2:]
        if directive == "no-log-check":
            assert not args, l
            do_log_check = False
        elif directive == "allow-difference":
            type, amount = args
            allowed_differences[type] = int(amount)
        elif directive == "on-failure-print":
            on_failure_messages.append(" ".join(args))
        else:
            raise Exception("Unknown test directive %r" % directive)

    result = parse_output(output)
    results_match = True
    if len(result) != len(expected):
        results_match = False
    else:
        for e, r in zip(expected, result):
            for k in set(e).union(r):
                if abs(e.get(k, 0) - r.get(k, 0)) > allowed_differences.get(k, 0):
                    print("Have %d vs %d %s" % (r.get(k, 0), e.get(k, 0), k))
                    results_match = False


    if results_match:
        print("Received expected output")
        if do_log_check:
            different = check_log(output, expected_log)

            # These checks are useful for making sure that we have the right expected
            # hashes in our test files, but I don't think it's worth failing the build for them:
            assert not different
            print("Log matches")
    else:
        if show_output:
            print('\n'.join(output.split('\n')[-500:]), file=sys.stderr)
            print('\n', file=sys.stderr)

        different = check_log(output, expected_log)

        print('\n', file=sys.stderr)
        print("WRONG output", file=sys.stderr)
        print("is:", result, file=sys.stderr)
        print("expected:", expected, file=sys.stderr)

        if do_log_check:
            if different:
                print("Log also doesn't match")
            else:
                print("Log matches")
        raise Exception("Results mismatch")

    print("Success!")
    return output

# Try to canonicalize the log to remove most spurious differences.
# We won't be able to get 100% of them, since there will always be differences in the number of
# python warnings or compiler messages.
# But try to remove the most egregious things (filename differences, timing differences) so that the output is easier to parse.
def process_log(log):
    log = log.split('\n')
    r = []

    # Remove all lines from creating virtualenvs, since 1) it's nondeterministic and 2) we expect there to
    # be differences between python builds (ex Pyston has to rebuild things since there aren't prebuilt wheels)
    no_virtualenv = []
    in_virtualenv = False
    for l in log:
        if l.startswith("created virtual environment"):
            in_virtualenv = True
        if not in_virtualenv:
            no_virtualenv.append(l)
        if l.startswith("Successfully installed"):
            in_virtualenv = False

    for l in no_virtualenv:
        # Remove timing data:
        l = re.sub("tests in ([\\d\\.])+s", "tests in XXXs", l)
        l = re.sub("in ([\\d\\.])+ seconds", "in XXX seconds", l)
        l = re.sub("in \\d+[.]\\d+s \\([0-9:]+\\)", "in XXXs ", l)
        l = re.sub("in \\d+[.]\\d+s ", "in XXXs ", l)

        # Remove filenames:
        # log = re.sub("/[^ ]*.py:\\d", "", log)
        # log = re.sub("/[^ ]*.py.*line \\d", "", log)
        if "http://" not in l:
            # Try to avoid running this expensive regex on really long lines.
            # So just run it on the first 300 characters and then join it back with the rest
            l1, l2 = l[:300], l[300:]
            l1 = re.sub(r"""(^|[ \"\'/]|\.+)[\w\d_\-./]*/[\w\d_\-./]*($|[ \":\',])""", "<filename>", l1)
            l = l1 + l2

        # Remove pointer ids:
        l = re.sub('0x([0-9a-f]{8,})', "<pointer>", l)

        # Remove timestamps
        l = re.sub("\\d\\d:\\d\\d:\\d\\d", "<timestamp>", l)

        # Normalize across python minor versions
        l = re.sub("Python 3.8.\\d+", "Python 3.8.x", l)

        # I think this isn't necessary since this isn't part of the passed log,
        # but pip installs packages in a nondeterministic order
        if l.startswith("Installing collected packages: "):
            words = l.split(' ')
            packages = [w.strip(',') for w in words[3:]]
            packages.sort()
            l = ' '.join(words[:3] + packages)

        # pytest prints out plugins in a random order
        if l.startswith("plugins: "):
            words = l.split(' ')
            plugins = [w.strip(',') for w in words[1:]]
            plugins.sort()
            l = ' '.join(words[:1] + plugins)

        # pytest prints out the environment variables and they are too long and get truncated differently
        # depending on the length of temp filename
        if l.startswith("kwargs = "):
            l = re.sub(r"'env': \{[^}]*", "'env': {<env>}", l)

        r.append(l)

    return r

def check_log(log, expected_log):
    orig_log_lines = log.split('\n')
    log_lines = process_log(log)

    expected_set = set(expected_log)
    missing = [l not in expected_set for l in log_lines]

    if log_lines == expected_log:
        return False

    for l in difflib.unified_diff(expected_log, log_lines):
        print(l)

    return True

def has_pyston_lite():
    try:
        import pyston_lite
    except:
        return False
    return True

def install_pyston_lite_into(py):
    path_to_pyston_lite_src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "pyston_lite")

    # speedup compilation
    env = os.environ.copy()
    env["NOPGO"] = "1"
    env["NOLTO"] = "1"
    env["NOBOLT"] = "1"

    subprocess.run([py, "setup.py", "install"], cwd=path_to_pyston_lite_src, env=env, capture_output=True)
    subprocess.run([py, "setup.py", "install"], cwd=os.path.join(path_to_pyston_lite_src, "autoload"), capture_output=True)


if __name__ == "__main__":
    if sys.argv[1] == "process_log":
        for l in sys.stdin:
            r = process_log(l[:-1])
            if r:
                print(r[0])
    elif sys.argv[1] == "parse_output":
        print(parse_output(sys.stdin.read()))
    elif sys.argv[1] == "compare":
        expected, actual = sys.argv[2:]
        expected_output = open(expected).read()
        actual_output = open(actual).read()

        compare(expected_output, actual_output, show_output=False)
    else:
        raise Exception("Unknown command %r" % sys.argv[1])

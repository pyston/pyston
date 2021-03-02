import os
import sys

sys.path.append(os.path.join(os.path.dirname(__file__), "../../Tools/clinic"))
from cpp import Monitor
sys.path.pop()

def processFile(filename):
    r = []
    with open(filename, "rt") as f:
        cpp = Monitor(filename, verbose=0)
        for line_number, line in enumerate(f.read().split('\n'), 1):
            pre_condition = cpp.condition()
            cpp.writeline(line)
            condition = cpp.condition()
            if "!defined(PYSTON_CLEANUP)" in condition:
                pass
            elif ("defined(PYSTON_CLEANUP)" in pre_condition) != ("defined(PYSTON_CLEANUP" in condition) or ("!defined(PYSTON_CLEANUP)" in pre_condition) != ("!defined(PYSTON_CLEANUP" in condition):
                pass
            else:
                r.append(line)
    return '\n'.join(r)

if __name__ == "__main__":
    if sys.argv[1] == "--dir":
        dir, = sys.argv[2:]
        for dirpath, dirnames, filenames in os.walk(dir):
            for filename in filenames:
                if not filename.endswith(".h"):
                    continue
                path = os.path.join(dirpath, filename)
                print(path)
                output = processFile(path)
                with open(path, "w") as f:
                    f.write(output)
    else:
        print(processFile(sys.argv[1]))

import glob
import os
import subprocess

if __name__ == "__main__":
    files = glob.glob(os.path.join(os.path.dirname(__file__), "*.normalized_ll"))
    files.remove("aot/aot_all.normalized_ll")
    files.remove("aot/all.normalized_ll")
    for fn1 in files:
        for fn2 in files:
            if fn1 >= fn2:
                continue
            # if fn1[:20] != fn2[:20]:
                # continue
            # if "BoolBool" not in fn1 or "BoolBool" not in fn2:
                # continue
            if "GetItem" not in fn1 or "GetItem" not in fn2:
                continue

            p = subprocess.Popen(["diff", "-U", "1", fn1, fn2], stdout=subprocess.PIPE)
            out, _ = p.communicate()
            ndiff = out.count(b'\n')
            if ndiff > 200:
                continue
            print(fn1, fn2, ndiff)
            if ndiff < 50:
                print(out.decode("utf8"))
                print()

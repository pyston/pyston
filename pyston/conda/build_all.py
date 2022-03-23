import collections
import json
import os
import queue
import random
import subprocess
import sys
import threading
import time

from make_order import getFeedstockOrder, getFeedstockName, getFeedstockDependencies
from pathlib import Path

SRC_DIR = Path(__file__).parent.absolute()

versions_to_build = {
    "numpy": ("1.18.1", "1.18.4", "1.18.5", "1.19.0", "1.19.1", "1.19.2", "1.19.4", "1.19.5", "1.20.0", "1.20.1", "1.20.2", "1.20.3", "1.21.0", "1.21.1", "1.21.2", "1.21.3", "1.21.4"),
    "tensorflow": ("2.4.3", "2.6.2"),
    "pytorch-cpu": ("1.8.0", "1.9.1", "1.10.0"),
    "pandas": ("1.3.4", "1.3.3", "1.3.2", "1.3.1", "1.3.0", "1.2.5", "1.2.4", "1.2.3", "1.2.2", "1.2.1", "1.2.0", "1.1.5"),

    "protobuf": ("3.15.8", "3.16.0", "3.18.1"),
    "wrapt": ("1.11.2", "1.12.1"), # pynput needs wrapt 1.11.*
    "h5py": ("2.10.0", "3.1.0"),
    "grpcio": "1.40.0",
    "setuptools": "57.4.0", # This is the version before they removed 2to3 support
    "pyyaml": "5.4.1",
    "docutils": "0.15.2",
    "astroid": "2.6.6", # <2.7 needed by pylint 2.9.6 needed by spyder
    "spyder-kernels": ("2.1.3", "2.2.1"), # <2.2.0 needed by some versions of spyder, >=2.2.1 required by others
    "torchvision": "0.10.1",
    "pandas": ("latest", "1.2.5"), # 1.2.5 needed by daal4py
    "keyring": "21.2.1", # 21.2.* needed by poetry
    "httpstan": "4.5.0", # 4.5.0 needed by pystan
    "setproctitle": ("1.1.10", "1.2.2"), # 1.1.10 needed by ray-packages
    "psycopg2": "2.9.3", # The next version (3.0.8) changed the library name in a way that broke some downstream packages like django
    "pydantic": "1.8.2", # thinc depends on pydantic <1.9.0
    "markupsafe": "2.0.1", # astropy needs this specific version
    "pytest": "6.2.5", # <7 is needed by anyio
    "ipython": ("7.30.0", "latest"), # <8.0 is needed by spyder
}

def getVersionsToBuild(feedstock):
    r = versions_to_build.get(feedstock, "latest")
    if isinstance(r, str):
        return (r,)
    return r

def splitIntoGroups(order, done_feedstocks, n=2):
    groups = {}
    for feedstock in order:
        if feedstock in done_feedstocks:
            continue

        deps = getFeedstockDependencies(feedstock)
        this_groups = set()
        for d in deps:
            # print(feedstock, d)
            if d not in order or d in done_feedstocks:
                continue
            if d not in groups:
                continue

            this_groups.update(groups[d])

        if len(this_groups) == 0:
            groups[feedstock] = set([len(groups)])
        else:
            groups[feedstock] = this_groups

    print(groups)

    group_size = collections.defaultdict(int)
    for k, v in groups.items():
        if len(v) == 1:
            group_size[list(v)[0]] += 1

    while True:
        best = None
        best_size = 1e9
        for k, v in groups.items():
            if len(v) == 1:
                continue
            s = sum([group_size[g] for g in v])
            print(k, [group_size[g] for g in v], s)
            if s < best_size:
                best_size = s
                best = k

        if not best:
            break

        replaced = groups[best]
        new_group = list(replaced)[0]
        print("Assigning", best, "to", new_group)

        for k, v in groups.items():
            new_set = set([new_group if g in replaced else g for g in v])
            if new_set == v:
                continue

            if len(v) == 1:
                group_size[list(v)[0]] -= 1
            if len(new_set) == 1:
                group_size[list(new_set)[0]] += 1
            groups[k] = new_set

        for g in replaced:
            if g == new_group:
                continue
            print("Done with group", g)
            assert group_size[g] == 0
            del group_size[g]
        assert group_size[new_group] >= best_size + 1, best_size

        print(groups)

    for k, v in group_size.items():
        assert v > 0, k

    splits = [set() for i in range(n)]
    while group_size:
        midx = None
        mval = 1e9
        for i, s in enumerate(splits):
            if len(s) < mval:
                mval = len(s)
                midx = i

        biggest_group = None
        bg_size = 0
        for k, v in group_size.items():
            if v > bg_size:
                bg_size = v
                biggest_group = k

        print("Putting group", biggest_group, "in split", midx)

        for k, v in groups.items():
            if biggest_group in v:
                splits[midx].add(k)
        del group_size[biggest_group]

    print()
    for i in range(n):
        print("Split", i)
        for feedstock in order:
            if feedstock in splits[i]:
                print(feedstock)
        print()

def buildAll(order, done, nparallel):
    added = set()
    done = set(done)
    failed = set()

    for feedstock in order:
        if feedstock in done:
            added.add(feedstock)
    q = queue.Queue()
    nidle = 0

    l = threading.Lock()
    def addReady():
        with l:
            for f in order:
                if f in added:
                    continue

                ready = True
                for d in getFeedstockDependencies(f):
                    if d not in order:
                        continue
                    if d in failed:
                        print(f, "can't be built because", d, "failed")
                        failed.add(f)
                        added.add(f)
                        ready = False
                        break

                    if d not in done:
                        # print(f, "is not ready because", d, "is not done")
                        ready = False
                        break

                if not ready:
                    continue

                print(f, "is ready to build, adding to queue")
                added.add(f)
                q.put(f)

    def run():
        while True:
            feedstock = q.get()
            if feedstock is None:
                return

            built_any = False
            for version in getVersionsToBuild(feedstock):
                if (feedstock, version) in done or (version == "latest" and feedstock in done):
                    continue

                built_any = True
                print("Building", feedstock, version)

                if feedstock in ("vtk", "fontconfig", "mamba", "boost"):
                    print("Known issue with %s, skipping for now" % feedstock)
                    code = 3
                else:
                    p = subprocess.Popen(["python3", "-u", SRC_DIR / "build_feedstock.py", feedstock, version, "--upload"], stdout=open("%s.log" % feedstock, "wb"), stderr=subprocess.STDOUT)
                    code = p.wait()

                print(feedstock, version, "finished with code", code)

                if code != 0:
                    break

                done.add((feedstock, version))

            if built_any:
                if code == 0:
                    done.add(feedstock)
                else:
                    failed.add(feedstock)
                addReady()

            q.task_done()

    addReady()
    for i in range(nparallel):
        t = threading.Thread(target=run)
        t.start()

    q.join()
    for i in range(nparallel):
        q.put(None)

def main():
    channel = "pyston"
    os.environ["CHANNEL"] = channel

    search_output = subprocess.check_output(["conda", "search", "*", "-c", channel, "--override-channels", "--json"]).decode("ascii")
    search_output = json.loads(search_output)

    done_feedstocks = set()
    for name, l in search_output.items():
        for d in l:
            if d["subdir"] == "noarch":
                continue
            feedstock = getFeedstockName(name)
            version = d["version"]
            done_feedstocks.add((feedstock, version))
            # TODO: better detection if there's a new version to build
            done_feedstocks.add((feedstock, "latest"))
            print("Found uploaded", feedstock, "(%s)" % name, version)

    topn = int(os.environ.get("TOPN", "1000"))
    targets = []
    for l in open(SRC_DIR / "package_list.txt"):
        targets.append(l.strip())
        if len(targets) >= topn:
            break
    for extra in ("conda", "uwsgi"):
        if extra not in targets:
            targets.append(extra)

    order = getFeedstockOrder(targets)

    for feedstock in order:
        all_done = True
        for version in getVersionsToBuild(feedstock):
            if (feedstock, version) not in done_feedstocks:
                all_done = False
                break
        if all_done:
            print("Done with", feedstock)
            done_feedstocks.add(feedstock)

    total = len(order)
    ntobuild = 0
    for feedstock in order:
        if feedstock not in done_feedstocks:
            ntobuild += 1

    print("Building %d / %d packages" % (ntobuild, total))

    for feedstock in order:
        if feedstock in done_feedstocks:
            # print(feedstock, "is done")
            continue

        deps = getFeedstockDependencies(feedstock)
        unbuilt_deps = [d for d in deps if (d in order and d not in done_feedstocks and d != feedstock)]
        if unbuilt_deps:
            print(feedstock, "depends on un-built", ", ".join(unbuilt_deps))
            continue

        print("Ready to build", feedstock)

    if "--split" in sys.argv:
        splitIntoGroups(order, done_feedstocks, 2)
        sys.exit()

    if "--parallel-build" in sys.argv:
        buildAll(order, done_feedstocks, nparallel=4)

if __name__ == "__main__":
    main()

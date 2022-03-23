import os
import shutil
import subprocess
import sys

from make_order import versionIsAtLeast

def getBranchToBuild(feedstock):
    if feedstock == "openmm":
        return "origin/rc"

    dir = "%s-feedstock" % feedstock

    subprocess.check_call(["git", "fetch", "origin"], cwd=dir)
    output = subprocess.check_output(["git", "branch", "-r"], cwd=dir).decode("utf8")
    if "origin/main" in output:
        return "origin/main"
    return "origin/master"

def findFeedstockCommitForVersion(feedstock, directory, version):
    base = getBranchToBuild(feedstock)
    if version == "latest":
        return base

    i = 0
    print("Finding the feedstock commit for version %s" % version)
    while True:
        commit = "%s~%d" % (base, i)
        s = subprocess.check_output(["git", "show", "%s:recipe/meta.yaml" % commit], cwd=directory).decode("utf8")
        print("Trying %s:" % commit, s.split('\n')[0])
        if 'version = "%s' % version in s:
            return commit
        i += 1

def getCherryPicks(feedstock, version):
    if feedstock == "numpy":
        return ("046882736", "6b1da6d7e", "4b48d8bb8", "672ca6f0d")

    return ()

def getRecipeSedCommands(feedstock, version):
    if feedstock == "python-rapidjson":
        return (r's/pytest tests/pytest tests --ignore=tests\/test_memory_leaks.py --ignore=tests\/test_circular.py/g',)

    if feedstock == "numpy":
        tests_to_skip = ["test_for_reference_leak", "test_api_importable"]
        if subprocess.check_output(["uname", "-m"]).decode("utf8").strip() == "aarch64":
            # https://github.com/numpy/numpy/issues/15243
            tests_to_skip.append("test_loss_of_precision")

        commands = [r's/_not_a_real_test/%s/g' % " or ".join(tests_to_skip)]

        if not versionIsAtLeast(version, "1.19.4"):
            commands.append(r's/^\(.\+\)sha256: \([a-f0-9]\+\)/\1sha256: \2\n\1patches:\n\1  - pyston.patch/')
        elif not versionIsAtLeast(version, "1.20.0"):
            commands.append(r's/^\(.\+\)patches:/\1patches:\n\1  - pyston.patch/')
        elif not versionIsAtLeast(version, "1.22.0"):
            commands.append(r's/^\(.\+\)sha256: \([a-f0-9]\+\)/\1sha256: \2\n\1patches:\n\1  - pyston.patch/')
        return commands

    if feedstock == "implicit":
        # The build step here implicitly does a `pip install numpy scipy`.
        # For CPython this will download a pre-built wheel from pypi, but
        # for Pyston it will try to recompile both of these packages.
        # But the meta.yaml doesn't include all the dependencies of
        # building scipy, specifically a fortran compiler, so we have to add it:
        return (r"s/        - {{ compiler('cxx') }}/        - {{ compiler('cxx') }}\n        - {{ compiler('fortran') }}/",)

    if feedstock == "pyqt":
        return (
                r"s@      - patches/qt5_dll.diff@      - patches/qt5_dll.diff\n      - pyston.patch@",
                r"s/qt >=5.12.9/qt 5.12.9 *_4/", # Later builds have an issue https://github.com/conda-forge/pyqt-feedstock/issues/108
        )

    if feedstock == "scikit-build":
        return (r"s/not test_fortran_compiler/not test_fortran_compiler and not test_get_python_version/",)

    if feedstock == "conda-package-handling":
        # Not sure why the sha mismatches, I think they must have uploaded a new version
        return (r"s/2a7cc4dc892d3581764710e869d36088291de86c7ebe0375c830a2910ef54bb6/6c01527200c9d9de18e64d2006cc97f9813707a34f3cb55bca2852ff4b06fb8d/",)

    if feedstock == "python-libarchive-c":
        # Not sure why the sha mismatches, I think they must have uploaded a new version
        return (r"s/bef034fbc403feacc4d28e71520eff6a1fcc4a677f0bec5324f68ea084c8c103/1223ef47b1547a34eeaca529ef511a8593fecaf7054cb46e62775d8ccc1a6e5c/",)

    if feedstock == "ipython":
        # ipython has a circular dependency via ipykernel,
        # but they have this flag presumably for this exact problem:
        if versionIsAtLeast(version, "8.0.1"):
            return (r"s/set migrating = false/set migrating = true/",)
        else:
            return (r"s/set migrating = False/set migrating = True/",)

    if feedstock == "gobject-introspection":
        # This feedstock lists a "downstream" test, which it will try to run
        # after this package is built.
        # Installing this dependent package will fail because we haven't built it yet;
        # conda is supposed to be able to recognize this and skip the tests, but it
        # it doesn't and instead fails.
        return (r"/pygobject/d",)

    if feedstock == "ipykernel":
        # ipykernel depends on ipyparallel for testing, but
        # ipyparallel depends on ipykernel
        return (r"/- ipyparallel/d",)

        # It's missing this dependency:
        # return ("/ipython_genutils/d" recipe/meta.yaml
        # return ("s/  host:/  host:\n    - ipython_genutils/" recipe/meta.yaml
        # return ("s/  run:/  run:\n    - ipython_genutils/" recipe/meta.yaml
        # return ("/debugpy/d" recipe/meta.yaml
        # return ("s/  host:/  host:\n    - debugpy >=1,<2.0/" recipe/meta.yaml
        # return ("s/  run:/  run:\n    - debugpy >=1,<2.0/" recipe/meta.yaml

    if feedstock == "nose":
        # Nose uses the setuptools "use_2to3" flag, which was removed in setuptools 58.0.0:
        return (r's/  host:/  host:\n    - setuptools <58/',)

    if feedstock == "jinja2-time":
        # This old package doesn't work with newer versions of arrow:
        return r('s/- arrow$/- arrow <0.14.5/',)

    if feedstock == "binaryornot":
        # This old package doesn't work with newer versions of hypothesis:
        return (r's/- hypothesis$/- hypothesis <4.0/',)
        # Though this doesn't work because apparently earlier versions of hypothesis are py27 only?
        # Though we don't actually need to build this feedstock because it became noarch

    if feedstock == "numba":
        # We have to disable some tests. They have some tracemalloc tests, and a test
        # that checks that shared libraries have "cpython" in the name.
        # They use unittest and I couldn't figure out how to disable them via the test runner
        return (r"s/  sha256: {{ sha256 }}/  sha256: {{ sha256 }}\n  patches:\n    - pyston.patch/",)

    if feedstock == "cython":
        if version == "latest":
            return ()
        assert version.startswith("0.29."), "not sure if this version requires our patch"
        if int(version.split(".")[2]) >= 25:
            return () # 0.29.25 contains our fix

        # we need to apply our memory corruption fix for cython #4200
        return (
                r"s/number: 1/number: 2\n  string: 2_pyston/g", # increment build number and add _pyston suffix so we know it's the fixed version
                r"s@    - patches/pypy37-eval.patch@    - patches/pypy37-eval.patch\n    - pyston.patch@",)

    if feedstock == "vim":
        return (r"/source:/a \  patches:\n    - pyston.patch",)

    if feedstock == "torchvision":
        assert version == "0.10.1", "other versions unsupported currently"
        r = []
        # torchvision 0.10.1 has a few test failures when run against pytorch 1.10 (all pass against 1.09)
        # disable this tests
        for t in ("test_distributed_sampler_and_uniform_clip_sampler", "test_random_clip_sampler" \
            "test_random_clip_sampler_unequal", "test_uniform_clip_sampler",  \
            "test_uniform_clip_sampler_insufficient_clips", "test_video_clips", "test_equalize[cpu]", \
            "test_read_timestamps", "test_read_timestamps_from_packet", "test_read_timestamps_pts_unit_sec", \
            "test_read_video_pts_unit_sec", "test_write_read_video", "test_write_video_with_audio", "test_compose"):
            r.append("/test_url_is_accessible\" %}/a \        {% set tests_to_skip = tests_to_skip + \" or ${t}\" %}")
        return r

    if feedstock == "tensorflow":
        if versionIsAtLeast("2.4.3", version):
            return (r"s/absl-py >=0.10.0/absl-py 0.10.0/",)

    if feedstock == "boost":
        return (r"s/patches:/patches:\n    - pyston.patch/",)

    if feedstock == "ruamel_yaml":
        # This package does 'pip install ruamel.yaml' (which is not the same package!),
        # which is somehow hardcoded to require gcc
        # return (r's/- {{ compiler("c") }}/- {{ compiler("c") }}\n    - gcc/',)
        return (r's/requires:/requires:\n    - gcc/',)

    if feedstock == "anyio":
        return (r's/pytest >=6.0/pytest >=6.0,<7/',)

    return ()

def getSedCommands(feedstock, version):
    recipe_cmds = getRecipeSedCommands(feedstock, version)
    r = [("recipe/meta.yaml", cmd) for cmd in recipe_cmds]

    if feedstock == "ipykernel":
        # We have to remove some of the tests that would call into it:
        # master:
        # sed -i 's/pytest_args += \[$/pytest_args += \["--ignore=" + os.path.dirname(loader.path) + "\/test_pickleutil.py",/' recipe/run_test.py
        r.append(("recipe/run_test.py", r's/print("Final pytest args:", pytest_args)/pytest_args += \["--ignore=" + os.path.dirname(loader.path) + "\/test_pickleutil.py"\]\nprint("Final pytest args:", pytest_args)/'))

    if feedstock == "boost":
        # r.append(("recipe/build.sh", r"s|include/python${PY_VER}|include/python${PY_VER}-pyston2.3|g"))
        r.append(("recipe/build.sh", r"s|${PY_VER}|${PY_VER}-pyston2.3|g"))

    if feedstock == "opencv":
        r.append(("recipe/build.sh", r"s|include/python${PY_VER}|include/python${PY_VER}-pyston2.3|g"))
        r.append(("recipe/build.sh", r"s|lib/libpython${PY_VER}|lib/libpython${PY_VER}-pyston2.3|g"))

    return r

def skipTests(feedstock):
    if os.environ.get("SKIP_TESTS", ""):
        return True
    return False

def buildFeedstock(feedstock, version="latest", do_upload=False):
    sed_commands = getSedCommands(feedstock, version)

    if not os.path.exists(feedstock + "-feedstock"):
        subprocess.check_call(["git", "clone", "https://github.com/conda-forge/%s-feedstock.git" % feedstock])

    dir = "%s-feedstock" % feedstock

    subprocess.check_call(["git", "reset", "--hard"], cwd=dir)
    subprocess.check_call(["git", "clean", "-fxd"], cwd=dir)
    subprocess.check_call(["git", "fetch"], cwd=dir)
    subprocess.check_call(["git", "checkout", findFeedstockCommitForVersion(feedstock, dir, version)], cwd=dir)

    for commit in getCherryPicks(feedstock, version):
        already_is_ancestor = (subprocess.call(["git", "merge-base", "--is-ancestor", commit, "HEAD"], cwd=dir) == 0)
        if not already_is_ancestor:
            print("Cherry-picking", commit)
            subprocess.check_call(["git", "cherry-pick", commit], cwd=dir)
        else:
            print("Skipping cherry-pick of", commit)

    # build_steps_script = open(os.path.join(dir, ".scripts/build_steps.sh"))
    # if "EXTRA_CB_OPTIONS" not in build_steps_scripts or "mambabuild" not in build_steps_scripts:
        # subprocess.check_call(["conda-smithy", "rerender"], cwd=dir)
    subprocess.check_call(["conda-smithy", "rerender"], cwd=dir)

    for file, cmd in sed_commands:
        tmp_str = "^^^^"
        if cmd[0] == 's':
            sep = cmd[1]
            grep_cmd = cmd.replace("\\" + sep, tmp_str)
            splits = grep_cmd.split(sep)
            assert len(splits) == 4, cmd
            check_pattern = splits[1]
            check_pattern = check_pattern.replace(tmp_str, sep)
        elif cmd[0] == '/':
            splits = cmd.split(cmd[0])
            assert len(splits) == 3, splits
            check_pattern = splits[0]
        else:
            assert 0, "Don't know how to interpret %r" % cmd

        # Convert sed pattern to egrep; these chars have opposite escaping rules:
        for chr in ("(", ")", "+", "$"):
            check_pattern = check_pattern.replace("\\" + chr, tmp_str)
            check_pattern = check_pattern.replace(chr, "\\" + chr)
            check_pattern = check_pattern.replace(tmp_str, chr)
        # Make sure the sed applies:
        subprocess.check_call(["egrep", "-q", check_pattern, file], cwd=dir)

        subprocess.check_call(["sed", "-i", cmd, file], cwd=dir)

    for name in ("%s%s.patch" % (feedstock, version), "%s.patch" % feedstock):
        patch_fn = os.path.join(os.path.dirname(__file__), "patches", name)
        if os.path.exists(patch_fn):
            print("Using patch file", patch_fn)
            shutil.copyfile(patch_fn, os.path.join(dir, "recipe/pyston.patch"))
            break

    env = dict(os.environ)
    env["CHANNEL"] = "pyston"
    config_file = subprocess.check_output(["python3", os.path.abspath(os.path.join(os.path.dirname(__file__), "make_config.py"))], env=env, cwd=dir)
    config_file = config_file.decode("utf8").strip()

    env = dict(os.environ)
    env["CONDA_FORGE_DOCKER_RUN_ARGS"] = "-e EXTRA_CB_OPTIONS --rm"
    env["EXTRA_CB_OPTIONS"] = "-c pyston"
    env["CI"] = ""
    if skipTests(feedstock):
        env["EXTRA_CB_OPTIONS"] += " --no-test"
    subprocess.check_call(["python3", "build-locally.py", config_file], cwd=dir, env=env)

    if do_upload:
        subprocess.check_call("anaconda upload -u pyston build_artifacts/*/*.tar.bz2", cwd=dir, shell=True)
        shutil.rmtree(os.path.join(dir, "build_artifacts"))

if __name__ == "__main__":
    subprocess.check_call(["which", "anaconda"])

    feedstock = sys.argv[1]

    do_upload = False
    if "--upload" in sys.argv:
        do_upload = True
        sys.argv.remove("--upload")

    version = "latest"
    if len(sys.argv) >= 3:
        version = sys.argv[2]

    buildFeedstock(feedstock, version, do_upload)

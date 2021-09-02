#!/usr/bin/python3
"""
This script either extracts a pyston debian package or takes an pyston install dir
and creates a portable version of it which contains all needed exernal shared objects.
"""
import glob
import subprocess
import re
import shutil
import os
import sys
from collections import namedtuple

VERBOSE = 0

class Dependency(namedtuple("Dependency", ["name", "path"])):
    def __eq__(self, other):
        return self.name == other.name

    def __hash__(self):
        return hash(self.name)

# this libraries will not get distributed
SkipLibs = {
    Dependency('libm.so.6', ''),
    Dependency('libc.so.6', ''),
    Dependency('libdl.so.2', ''),
    Dependency('libpthread.so.0', ''),
    Dependency('linux-vdso.so.1', ''),

    # _tkinter.pyston-20-x86_64-linux-gnu.so has many dependencies:
    Dependency('libBLT.2.5.so.8.6', ''),
    Dependency('libtk8.6.so', ''),
    Dependency('libtcl8.6.so', ''),
    Dependency('libX11.so.6', ''),
    Dependency('libXft.so.2', ''),
    Dependency('libfontconfig.so.1', ''),
    Dependency('libXss.so.1', ''),
    Dependency('libxcb.so.1', ''),
    Dependency('libfreetype.so.6', ''),
    Dependency('libXrender.so.1', ''),
    Dependency('libXext.so.6', ''),
    Dependency('libXau.so.6', ''),
    Dependency('libXdmcp.so.6', ''),
}

def get_dependencies(filepath):
    o = subprocess.check_output(["ldd", filepath]).decode('utf-8')
    dependencies = []
    if VERBOSE:
        print(f"looking at dependency of: {filepath}")

    # raise an exception if we can't find a dependency
    for entry in re.findall(r"\t(.*) => not found", o):
        raise Exception(f"could not find {entry} required by {filepath}")

    for entry in re.findall(r"\t(.*) => (.*) (\(.*\))?", o):
        if VERBOSE:
            print("  found", entry)
        dependencies.append(Dependency(entry[0], entry[1]))
    return dependencies


def recursive_get_dependencies(entry):
    visited = set()
    visit_next = {entry}

    changed = True

    while changed:
        changed = False

        visit_now = visit_next
        visit_next = set()

        for x in visit_now:
            if x in visited or x in SkipLibs:
                continue
            visited.add(x)
            merged = visit_next | (set(get_dependencies(x.path)) - SkipLibs)
            if merged != visit_next:
                changed = True
                visit_next = merged

    # remove entry and libs which should be skipped
    return visited - SkipLibs - {entry}


def copy_files(dependencies, outdir):
    try:
        os.makedirs(outdir)
    except OSError:
        pass

    for dependency in dependencies:
        shutil.copy2(dependency.path, outdir)


def set_rpath(f, rpath):
    subprocess.check_call(["patchelf", "--set-rpath", rpath, f])


def is_so(filename):
    return filename.endswith(".so")

def make_portable_dir(outdir):
    dependencies = recursive_get_dependencies(
        Dependency("pyston", outdir + "/usr/bin/pyston"))

    # get output lib directory
    paths = glob.glob(outdir + "/usr/lib/python3.8-pyston*/lib-dynload/")
    assert len(paths) == 1 and paths[0].endswith("/")
    path = paths[0]

    for f in filter(is_so, os.listdir(path)):
        dependencies |= recursive_get_dependencies(Dependency(f, path + f))
        # we like to use our supplied .so's only as fallback if the dist ones are not available
        # because ours are likely older which causes problems.
        # best way to do it seems to put the most common dist lib paths in front of
        # the path to our own libs.
        set_rpath(path + f, "/lib64:/usr/lib/x86_64-linux-gnu:$ORIGIN/../../../lib")

    for x in dependencies:
        print(f"copy {x} into lib/")
        copy_files(dependencies, outdir + "/usr/lib")

    # remove pip wrappers which will not work because of hardcoded #!path
    for f in glob.glob(outdir + "/usr/bin/pip*"):
        print(f"removing {f}")
        os.remove(f)

    # create toplevel symlinks for quick access
    for f in glob.glob(outdir + "/usr/bin/pyston*"):
        src = os.path.relpath(f, outdir)
        dst = os.path.join(outdir, os.path.basename(f))
        print(f"creating symlink {dst} -> {src}")
        os.symlink(src, dst)

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("USAGE:")
        print(f"\t{sys.argv[0]} <input.deb> <output_dir_name>")
        print("usage for quick tests but not for wide distribution:")
        print(f"\t{sys.argv[0]} <input_install_dir_name> <output_dir_name>")
    else:
        outdir = sys.argv[2]
        
        if sys.argv[1].endswith(".deb"):
            os.mkdir(outdir) # create output dir, will raise exception if it already exists
            subprocess.check_call(["dpkg", "-x", sys.argv[1], sys.argv[2]])
        else:
            print("WARNING: Creating portable packages from an install dir is only recommended for internal distribution.")
            print("         Because they are likely not stripped and contain additional (temp) files.")
            shutil.copytree(sys.argv[1], outdir)
        make_portable_dir(outdir)


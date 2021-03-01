import ctypes
import os
import subprocess
import sys

if __name__ == "__main__":
    filename = os.path.abspath(sys.argv[1])
    funcnames = sys.argv[2:]
    if not funcnames:
        print("Usage: python test_optimize.py FILENAME FUNCNAME+")
        sys.exit(1)

    os.chdir(os.path.join(os.path.dirname(__file__), ".."))

    if filename.endswith(".c") or filename.endswith(".cpp"):
        new_fn = filename.rsplit(".c", 1)[0] + ".ll"
        if not os.path.exists(new_fn) or os.stat(new_fn).st_mtime < os.stat(filename).st_mtime:
            args = ["build/Release/llvm/bin/clang-10", "-g", "-O3", "-Ibuild/cpython_bc_install/include/python3.8", "-DNDEBUG", "-Wall", "-c", "-emit-llvm", "-S", filename]
            print(' '.join(args))
            subprocess.check_call(args)
        filename = new_fn

    nitrous_so = ctypes.PyDLL("libinterp.so")
    loadBitcode = nitrous_so.loadBitcode
    loadBitcode.argtypes = [ctypes.c_char_p]

    link_fn = filename + ".link.bc"
    if not os.path.exists(link_fn) or os.stat(link_fn).st_mtime < os.stat(filename).st_mtime:
        args = ["build/Release/llvm/bin/llvm-link", "aot/all.bc", filename, "-o", link_fn]
        print(" ".join(args))
        subprocess.check_call(args)
    loadBitcode(link_fn.encode("ascii"))

    initializeJIT = nitrous_so.initializeJIT
    initializeJIT.argtypes = [ctypes.c_long]
    initializeJIT(3)

    pystol_so = ctypes.PyDLL("libpystol.so")
    pystol_so.pystolGlobalPythonSetup()

    optimize = nitrous_so["optimizeBitcode"]
    optimize.argtypes = [ctypes.c_char_p]
    for funcname in funcnames:
        optimize(funcname.encode("ascii"))

#!/usr/bin/env python3

import os
import subprocess
import sys

def main():
    cc = os.environ["WRAPPER_REALCC"]

    output_prefix = os.environ.get("WRAPPER_OUTPUT_PREFIX", None)
    output_suffix = os.environ.get("WRAPPER_OUTPUT_SUFFIX", None)
    assert output_prefix is not None or output_suffix is not None, "specify one of WRAPPER_OUTPUT_PREFIX or WRAPPER_OUTPUT_SUFFIX"
    output_prefix = output_prefix or ""
    output_suffix = output_suffix or ""

    if '-c' not in sys.argv or "conftest.c" in sys.argv:
        os.execv(cc, [cc] + sys.argv[1:])
        # not reached
        raise Exception()

    args = sys.argv[1:]

    emit_args = list(args)
    emit_args.append("-emit-llvm")

    for i in range(len(emit_args) - 1):
        if emit_args[i] != '-o':
            continue

        normal_output = emit_args[i + 1]
        dn = os.path.dirname(normal_output)
        bc_output = os.path.join(output_prefix, dn, output_suffix, os.path.basename(normal_output)) + ".bc"
        emit_args[i + 1] = bc_output
        break
    else:
        raise Exception("couldn't determine output file")

    # if we build a conda package make sure that we have not accidently included a system header
    if os.environ.get("CONDA_BUILD"):
        for arg in args:
            if arg.startswith("-I"):
                arg = arg[2:]
            if arg.startswith("/usr/include") or arg.startswith("/usr/local/include"):
                raise Exception("compiler uses system include " + arg)

    os.makedirs(os.path.dirname(bc_output), exist_ok=True)

    # print >>sys.stderr, normal_output, bc_output

    subprocess.check_call([cc] + emit_args)
    compile_args = ["-O3"]
    if "-fPIC" in args:
        compile_args.append("-fPIC")

    """
    We have two options for how to produce the .o file:
    1) We can re-compile the C file using the original args
    2) We can lower our created bitcode to an object file

    Option 2 seems nicer, because it minimizes the distance between
    the file we import (the bitcode file) and the file we execute
    (the .o file).

    However it's a bit tricky to do this: the easiest way is to
    run the bitcode file through clang.  This has the problem, though,
    of using the same bitcode-optimization flags as bitcode-lowering
    flags.  So to get the bitcode-lowering optimizations that seem to
    be important (todo: I forgot how I tested this), we have to pass
    -O3, but this ends up reoptimizing the generated bitcode and can
    change its structure (such as inlining and eliminating internal
    functions).

    llc seems to do similar optimizations.

    So for now it seems like recompiling from the C file results
    in bitcode and object files that are the most similar.
    """
    if 0:
        # Compile bitcode->.o
        os.execv(cc, [cc, bc_output, "-c", "-o", normal_output] + compile_args)
    else:
        # Compile .c->.o
        os.execv(cc, [cc] + args)

if __name__ == "__main__":
    main()

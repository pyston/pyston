#!/usr/bin/env python3
#
# Preprocesses the DynASM file to remove architecture specific code depending on local architecture.
# Non matching architecture code is replaced with empty lines so that line numbers match.
# Usage:
#   python3 dynasm_preprocess.py <input_file> <output_file>
#
# Currently supported architectures:
#   ARM -> aarch64
#   X86 -> x86_64
#
# Supported directives:
#  Directives must be first nonwhite space characters on the line.
#  @<ARCH><CODE> # removes <CODE> depending on arch
#  @<ARCH>_START # removes all lines to the next @<ARCH>_END depending on arch
#  @<ARCH>_END
#
# Example single line directive:
#  @ARM| .define name, reg_arm64
#  @ARM|| #define name_idx reg_arm64_idx
#  @X86| .define name, reg_amd64
#  @X86|| #define name_idx reg_amd64_idx
#
# Example multi line directive:
#   @X86_START
#       if (val == 0) {
#           | test Rd(r_idx), Rd(r_idx)
#       } else if (IS_32BIT_VAL(val)) {
#           | cmp Rd(r_idx), (unsigned int)val
#       } else {
#           JIT_ASSERT(0, "should not reach this");
#       }
#   @X86_END
#
# Without this tool the code in the single line example would look like:
#  |.if ARCH=aarch64
#  #ifdef __aarch64__
#   | .define name, reg_arm64
#   #define name_idx reg_arm64_idx
#  #endif
#  |.else
#  #ifndef __aarch64__
#   | .define name, reg_amd64
#   #define name_idx reg_amd64_idx
#  #endif
#  |.endif
#
# Because the DynASM |.if/|.endif directives only remove DynASM code and not C code
# so we also always have to wrap it an additional #ifdef/#endif pair to remove the C code
# (e.g. calls to emit_mov_imm() would not get removed by |.if/|.endif).
# And just using the preprocessor would not work because DynASM runs before the C preprocessor
# and will complain about the different achitecture assembler instructions.

import platform
import sys

ARCHS = {
    "aarch64": "ARM",
    "arm64": "ARM", # macOS name
    "x86_64": "X86",
}


def preprocess(filename_in, filename_out):
    def raise_exc(msg):
        raise Exception("Error on line %s: %s" % (lineno, msg))

    local_arch = ARCHS[platform.machine()]
    with open(filename_out, "w") as file:
        # this is set to the directive name if we are inside a multi line directive to catch
        # unmatched pairs
        inside_multiline_directive = None
        # should we skip the next lines?
        skip = False
        for lineno, line in enumerate(open(filename_in, "r").readlines(), 1):
            line_stripped = line.strip()
            if line_stripped.startswith("@"):
                # we go over all supported archs because it makes it easier to catch errors
                for arch in ARCHS.values():
                    directive = "@" + arch
                    if line_stripped.startswith(directive):
                        directive_start = directive + "_START"
                        directive_end = directive + "_END"

                        if line_stripped.startswith(directive_start):
                            if inside_multiline_directive:
                                raise_exc(
                                    "Found %s inside %s"
                                    % (directive_start, inside_multiline_directive)
                                )
                            inside_multiline_directive = directive_start
                            skip = local_arch != arch
                            file.write("\n")
                        elif line_stripped.startswith(directive_end):
                            if inside_multiline_directive != directive_start:
                                raise_exc(
                                    "Mixed %s with %s"
                                    % (inside_multiline_directive, directive_end)
                                )
                            inside_multiline_directive = None
                            skip = False
                            file.write("\n")
                        else:
                            if inside_multiline_directive:
                                raise_exc(
                                    "Found %s inside %s"
                                    % (directive, inside_multiline_directive)
                                )
                            if arch == local_arch:
                                file.write(
                                    line.replace(directive, " " * len(directive))
                                )
                            else:
                                file.write("\n")
                        break
                else:
                    raise_exc("Unknown architecture: " + line_stripped)
            else:
                file.write("\n" if skip else line)
                if line_stripped.startswith("|") or (line_stripped.startswith("#") and '\\' not in line_stripped):
                    file.write('#line %d "%s"\n' % (lineno + 1, filename_in))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: %s <input.c> <output.c>" % (sys.argv[0],))
        sys.exit(1)
    preprocess(filename_in=sys.argv[1], filename_out=sys.argv[2])

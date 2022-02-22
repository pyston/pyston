#!/usr/bin/python2
import sys, subprocess
import traceback

import argparse
import commands
import os
import platform
import re
import subprocess
import sys

def get_objdump(args):
    if "/tmp/perf-" in args[-1]:
        start_addr = 0
        for arg in args:
            if "--start-address=" in arg:
                start_addr = int(arg[len("--start-address="):], 16)

        for l in open(args[-1]):
            l = l.split()
            if len(l) != 3:
                # Some function names have spaces in them
                continue
            addr, size, name = l
            if int(addr, 16) == start_addr:
                arch = platform.machine()
                if arch == "x86_64":
                    arch = "i386:x86-64"
                p = subprocess.Popen(["objdump", "-b", "binary", "-m", arch, "-l", "-C", "-D", "/tmp/perf_map/" + name, "--adjust-vma=0x%s" % addr, "--no-show-raw"], stdout=subprocess.PIPE)
                break
        else:
            raise Exception("Couldn't find address %s" % addr)
    else:
        p = subprocess.Popen(["objdump"] + args, stdout=subprocess.PIPE)
    r = p.communicate()[0]
    assert p.wait() == 0
    return r


def getBinaryName():
    for arg in sys.argv:
        if ".debug/.build-id/" in arg:
            return arg
    return open("/tmp/perf_map/executable.txt").read()

_symbols = None
def lookupAsSymbol(n):
    global _symbols
    if _symbols is None:
        _symbols = {}
        nm_output = commands.getoutput("nm " + getBinaryName())
        for l in nm_output.split('\n'):
            addr = l[:16]
            if addr.isalnum():
                _symbols[int(addr, 16)] = l[19:]
    return _symbols.get(n, None)

def lookupConstant(n):
    sym = lookupAsSymbol(n)
    if sym:
        return "; " + sym
    return ""

_opcode_map = None
def addrToOpcodeName(opcode_addr):
    global _opcode_map
    if _opcode_map is None:
        _opcode_map = {}
        for l in open("/tmp/perf_map/opcode_map.txt").readlines():
            s = l.strip().split(",")
            if len(s) < 2:
                continue
            addr = int(s[0], 16)
            op = s[1]
            if addr not in _opcode_map:
                _opcode_map[addr] = list()
            _opcode_map[addr].append(op)
    return _opcode_map.get(opcode_addr, None)

def getCommentForInst(inst, index, insts):
    patterns = ["movabs \\$?0x([0-9a-f]+),",
                "mov    \\$?0x([0-9a-f]+),",
                "movq   \\$?0x([0-9a-f]+),",
                "cmp    \\$?0x([0-9a-f]+),",
                "cmpq   \\$?0x([0-9a-f]+),",
                "callq  0x([0-9a-f]+)$",
                ]

    for pattern in patterns:
        m = re.search(pattern, inst)
        if m:
            n = int(m.group(1), 16)
            if n:
                return lookupConstant(n)

    # on ARM64 we need up to 4 instructions to load a 64bit immediate:
    # mov to set the lowest 16bit and clear the top followed by movk instructions:
    # mov   w1, #0x83d0
    # movk  x1, #0xb38b, lsl #16
    # movk  x1, #0xffff, lsl #32
    mov = re.search(r"mov\s+[wx](\d{1,2}), #(0x[a-f0-9]+)", inst)
    if mov:
        n = int(mov.group(2), 16)
        num_movk = 0
        for l in lines[index+1:]:
            movk = re.search(r"movk\s+x" + mov.group(1) + ", #(0x[a-f0-9]+), lsl #(\d\d)", l)
            if not movk:
                break
            n += int(movk.group(1), 16) << int(movk.group(2))
            num_movk += 1
        if num_movk:
            c = lookupConstant(n)
            return c if c else "; " + hex(n)

    return None

def getAddressOfInst(inst):
    s = inst.strip().split(":")
    try:
        if len(s)>=2:
            n = int(s[0], 16)
            if n:
                return n
    except:
        pass

def replaceInst(inst):
    # we replace relative call instructions with the symbol name
    m = re.search("callq  (0x[0-9a-f]+)$", inst)
    if m:
        n = int(m.group(1), 16)
        if n and lookupAsSymbol(n):
            return inst + " <" + lookupAsSymbol(n) + ">"
    return inst

if __name__ == "__main__":
    try:
        objdump = get_objdump(sys.argv[1:])
        lines = [l for l in objdump.split('\n')[7:]]

        for i, l in enumerate(lines):
            l = replaceInst(l)

            # check if we have opcode info string for current address
            addr = getAddressOfInst(l)
            if addr:
                opcodes = addrToOpcodeName(addr)
                if opcodes:
                    print "" # empty line to make it easier to spot the start of new opcode
                    for op in opcodes:
                        print "; ", op

            extra = getCommentForInst(l, i, lines) or ""
            print l.ljust(70), extra
    except:
        traceback.print_exc(file=open("/tmp/perf_jit_exc.txt", "w"))
        raise

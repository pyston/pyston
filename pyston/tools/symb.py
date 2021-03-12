#!/usr/bin/python3
# replaces pointer addresses in stdio with the symbol name frome the cpython executable
# usage: enable NITROUS_JIT_VERBOSE inside src/common.h
#        make prime_summing | tools/symb.py

import sys
import re
import subprocess

syms = []

def loadSymData(filename, offset):
    symdata = subprocess.getoutput("nm -S %s" % filename)

    if symdata.endswith("no symbols"):
        return

    for l in symdata.split('\n'):
        words = l.split()
        if len(words) != 4:
            continue
        start = int(words[0], 16) + offset
        size = int(words[1], 16)
        name = words[3]
        syms.append((start, start + size, name))

def getSymName(addr):
    for start, end, sym in syms:
        if addr == start:
            return sym
        if start <= addr < end:
            return sym + "+0x%x" % (addr - start)
    return None

for line in sys.stdin:
    if line.startswith("symb load "):
        words = line.split(' ', 3)
        offset = int(words[2], 16)
        filename = words[3]
        loadSymData(filename, offset)
        continue

    pat = r"i64 ([0-9]+)"
    m = re.search(pat, line)
    if m and len(m.group(1)) > 5: # check that it is at least 5 digits long
        sym = getSymName(int(m.group(1)))
        if sym:
            line = re.sub(pat, "'" + sym + "'", line)
    print(line, end='')


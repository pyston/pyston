"""
 Import this script inside gdb via the 'source tools/dis_jit_gdb.py' command
 Afterwards it will display for every newly JIT compiled function
 the python opcode followed by the generated machine code.
 It does not show prolog/epilog of the emitted function only the code for the bytecodes.
"""
import dis

def stop_handler(event):
    if not isinstance(event, gdb.BreakpointEvent):
        return

    # we use dasm_free instead of jit_func because it gets called from jit_func at a convient place
    # and I could not find a way to automatically set a breakpoint at the end of jit_func before it's returning
    if event.breakpoint.location != "dasm_free":
        return

    gdb.execute("frame 1")
    co = gdb.parse_and_eval("co")
    def getPythonStr(cmd):
        return str(gdb.parse_and_eval(f"PyUnicode_AsUTF8({cmd})")).split('"')[1]
    print(f"Dissasembly of {getPythonStr('co->co_filename')}:{int(gdb.parse_and_eval('co->co_firstlineno'))} {getPythonStr('co->co_name')}")

    num_opcodes = int(gdb.parse_and_eval("jit.num_opcodes"))
    for i in range(num_opcodes):
        codeunit = int(gdb.parse_and_eval(f"jit.first_instr[{i}]"))
        opcode = codeunit & 0xFF
        oparg = codeunit >> 8
        print(f"{i*2:8} {dis.opname[opcode]:30} {oparg:10}")

        flags = "" # "/r" to see machine code bytes too
        until = f"((int*)(labels[lbl_opcode_addr_begin]))[{i}+1]" if i+1 < num_opcodes else "labels[lbl_epilog]"
        gdb.execute(f"disassemble {flags} ((int*)(labels[lbl_opcode_addr_begin]))[{i}], {until}")

gdb.events.stop.connect(stop_handler)
gdb.Breakpoint("dasm_free")

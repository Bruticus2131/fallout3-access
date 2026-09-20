"""Locate an ObScript command added by CommandExtender.dll and disassemble its
handler.

FOSE plugins register commands as CommandInfo structs:
    +00 const char* longName      +14 ParamInfo* params
    +04 const char* shortName     +18 Cmd_Execute execute
    +08 UInt32 opcode             +1C Cmd_Parse parse
    +0C const char* helpText      +20 Cmd_Eval eval
    +10 UInt16 needsParent        +24 UInt32 flags
    +12 UInt16 numParams

So: find the command NAME string, find the pointer to it inside the DLL's data
(that pointer is the CommandInfo's first field), then read the struct and
disassemble `execute`. Tells us what game function the command actually calls,
which is the part we can reuse directly.

Usage:  python find_ce_command.py FaceObject [instruction-count]
"""
import sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

DLL = r"D:\SteamLibrary\steamapps\common\Fallout 3 goty\Data\fose\plugins\CommandExtender.dll"


def main():
    name = sys.argv[1] if len(sys.argv) > 1 else "FaceObject"
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 60

    pe = pefile.PE(DLL)
    base = pe.OPTIONAL_HEADER.ImageBase
    data = pe.get_memory_mapped_image()

    needle = name.encode() + b"\x00"
    str_rvas = []
    start = 0
    while True:
        i = data.find(needle, start)
        if i < 0:
            break
        str_rvas.append(i)
        start = i + 1
    if not str_rvas:
        print("string %r not found" % name)
        return
    print("name string at RVA(s): %s" % ", ".join(hex(r) for r in str_rvas))

    # Find a 4-byte little-endian pointer to the string = CommandInfo.longName.
    for srva in str_rvas:
        ptr = (base + srva).to_bytes(4, "little")
        pos = 0
        while True:
            i = data.find(ptr, pos)
            if i < 0:
                break
            pos = i + 1
            ci = data[i:i + 0x28]
            if len(ci) < 0x28:
                continue
            opcode = int.from_bytes(ci[0x08:0x0C], "little")
            numparams = int.from_bytes(ci[0x12:0x14], "little")
            execute = int.from_bytes(ci[0x18:0x1C], "little")
            params = int.from_bytes(ci[0x14:0x18], "little")
            # Sanity: a plausible CommandInfo has a code pointer and few params.
            if not (base < execute < base + len(data)) or numparams > 8:
                continue
            print("\nCommandInfo at RVA 0x%X (VA 0x%X)" % (i, base + i))
            print("  opcode    = 0x%X" % opcode)
            print("  numParams = %d" % numparams)
            print("  params    = 0x%X" % params)
            print("  execute   = 0x%X (RVA 0x%X)" % (execute, execute - base))

            # Parameter types, if any: ParamInfo = {const char* name, UInt32 type,
            # UInt32 isOptional}
            prva = params - base
            for p in range(numparams):
                off = prva + p * 12
                pname_ptr = int.from_bytes(data[off:off + 4], "little")
                ptype = int.from_bytes(data[off + 4:off + 8], "little")
                popt = int.from_bytes(data[off + 8:off + 12], "little")
                pn = data[pname_ptr - base:pname_ptr - base + 40].split(b"\x00")[0]
                print("    param %d: %-16s type=0x%X optional=%d"
                      % (p, pn.decode(errors="replace"), ptype, popt))

            md = Cs(CS_ARCH_X86, CS_MODE_32)
            code = data[execute - base: execute - base + count * 8]
            print("  --- execute disassembly ---")
            for ins in list(md.disasm(code, execute))[:count]:
                print("    0x%X:\t%s\t%s" % (ins.address, ins.mnemonic, ins.op_str))
            return

    print("no CommandInfo pointing at the name string was found")


main()

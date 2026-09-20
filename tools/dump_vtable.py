"""Dump a vtable from Fallout3.exe: each slot's address plus its first few
instructions, so the slot we want can be recognised by shape.

Usage: python dump_vtable.py 0x00E1EE04 [slots]
"""
import os, sys, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

EXE = os.environ.get("F3_EXE",
    r"D:\SteamLibrary\steamapps\common\Fallout 3 goty\Fallout3.exe")

vt = int(sys.argv[1], 16)
n = int(sys.argv[2]) if len(sys.argv) > 2 else 16

pe = pefile.PE(EXE)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
md = Cs(CS_ARCH_X86, CS_MODE_32)

for i in range(n):
    off = vt - base + i * 4
    fn = int.from_bytes(img[off:off + 4], "little")
    print("slot %2d (+0x%02X): 0x%08X" % (i, i * 4, fn))
    if not (base < fn < base + len(img)):
        print("      <not a code pointer>")
        continue
    code = img[fn - base: fn - base + 40]
    for ins in list(md.disasm(code, fn))[:5]:
        print("      %s %s" % (ins.mnemonic, ins.op_str))

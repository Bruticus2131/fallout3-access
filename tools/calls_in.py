"""List CALL targets inside a function in Fallout3.exe, with the pushes right
before each call — enough to recognise a call by its argument shape.

Usage: python calls_in.py 0x007E8950 [instruction-budget]
"""
import os, sys, pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

EXE = os.environ.get("F3_EXE",
    r"D:\SteamLibrary\steamapps\common\Fallout 3 goty\Fallout3.exe")
addr = int(sys.argv[1], 16)
budget = int(sys.argv[2]) if len(sys.argv) > 2 else 400

pe = pefile.PE(EXE)
base = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
md = Cs(CS_ARCH_X86, CS_MODE_32)

code = img[addr - base: addr - base + budget * 8]
ins = list(md.disasm(code, addr))[:budget]
for i, x in enumerate(ins):
    if x.mnemonic != "call":
        continue
    ctx = [f"{y.mnemonic} {y.op_str}" for y in ins[max(0, i - 5):i]]
    print("0x%X: call %s   <= %s" % (x.address, x.op_str, " | ".join(ctx)))

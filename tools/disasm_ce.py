"""Disassemble an arbitrary address inside CommandExtender.dll.

Usage: python disasm_ce.py 0x10004ef0 [count]
"""
import sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

DLL = r"D:\SteamLibrary\steamapps\common\Fallout 3 goty\Data\fose\plugins\CommandExtender.dll"

addr = int(sys.argv[1], 16)
count = int(sys.argv[2]) if len(sys.argv) > 2 else 60

pe = pefile.PE(DLL)
base = pe.OPTIONAL_HEADER.ImageBase
data = pe.get_memory_mapped_image()

md = Cs(CS_ARCH_X86, CS_MODE_32)
code = data[addr - base: addr - base + count * 8]
for ins in list(md.disasm(code, addr))[:count]:
    print("0x%X:\t%s\t%s" % (ins.address, ins.mnemonic, ins.op_str))

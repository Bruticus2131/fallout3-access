import pefile, struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

import os
EXE = os.environ.get("F3_EXE", r"D:\SteamLibrary\steamapps\common\Fallout 3 goty\Fallout3.exe")
pe = pefile.PE(EXE, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = pe.get_memory_mapped_image()
md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = False

def disasm(va, n, tag=""):
    rva = va - base
    code = data[rva:rva+n]
    print("=== 0x%08X %s ===" % (va, tag))
    for ins in md.disasm(code, va):
        mark = ""
        if ins.mnemonic == "call":
            mark = "   <<< CALL"
        if ins.mnemonic.startswith("mov") and "ptr" in ins.op_str and ins.op_str.index("ptr") < ins.op_str.find(","):
            mark = "   <<< STORE"
        print("  0x%08X  %-9s %s%s" % (ins.address, ins.mnemonic, ins.op_str, mark))

va = int(sys.argv[1], 16) if len(sys.argv) > 1 else 0x0052DC00
n  = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x200
disasm(va, n, "objective cmd")

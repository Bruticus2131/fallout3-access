import pefile, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Fallout 3\Fallout3.exe"
pe = pefile.PE(EXE, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = pe.get_memory_mapped_image()
md = Cs(CS_ARCH_X86, CS_MODE_32)

def disasm(va, n, tag=""):
    rva = va - base
    code = data[rva:rva+n]
    print("=== 0x%08X %s ===" % (va, tag))
    for ins in md.disasm(code, va):
        mark = ""
        if ins.mnemonic == "call":
            mark = "   <<< CALL"
        print("  0x%08X  %-9s %s%s" % (ins.address, ins.mnemonic, ins.op_str, mark))

disasm(0x0050EF90, 130, "GetAV helper 0x50EF90")

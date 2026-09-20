import pefile, struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Fallout 3\Fallout3.exe"
pe = pefile.PE(EXE, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = pe.get_memory_mapped_image()
secs = [(s.Name.rstrip(b"\x00").decode("latin1"), base + s.VirtualAddress,
         base + s.VirtualAddress + s.Misc_VirtualSize) for s in pe.sections]
text = next(s for s in secs if s[0] == ".text")
def in_text(va): return text[1] <= va < text[2]
md = Cs(CS_ARCH_X86, CS_MODE_32)

def back_disasm(va, span=48):
    # disassemble a window ending near va by scanning a bit before
    start = va - span
    code = data[start-base: va-base+8]
    lines=[]
    for ins in md.disasm(code, start):
        lines.append("    0x%08X %-8s %s" % (ins.address, ins.mnemonic, ins.op_str))
    return lines[-8:]

def find_refs(value):
    pat = struct.pack("<I", value); off=0; out=[]
    while True:
        i = data.find(pat, off)
        if i<0: break
        va = base+i
        if in_text(va): out.append(va)
        off=i+1
    return out

for arg in sys.argv[1:]:
    val = int(arg,16)
    refs = find_refs(val)
    print("\n#### refs to 0x%08X in .text: %d ####" % (val, len(refs)))
    for r in refs[:12]:
        print("  REF @ 0x%08X" % r)
        for l in back_disasm(r): print(l)

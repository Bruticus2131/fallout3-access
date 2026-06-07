import pefile, struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Fallout 3\Fallout3.exe"
NAME = sys.argv[1].encode() + b"\x00" if len(sys.argv) > 1 else b"GetActorValue\x00"
pe = pefile.PE(EXE, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = pe.get_memory_mapped_image()
secs = [(s.Name.rstrip(b"\x00").decode("latin1"), s.VirtualAddress,
         s.VirtualAddress + s.Misc_VirtualSize) for s in pe.sections]
text = next(s for s in secs if s[0] == ".text")
def in_text(va): return text[1] <= va - base < text[2]

str_vas = []
off = 0
while True:
    i = data.find(NAME, off)
    if i < 0: break
    if i == 0 or data[i-1] == 0: str_vas.append(base + i)
    off = i + 1
print("string %r VAs: %s" % (NAME, [hex(v) for v in str_vas]))

execs = []
for sva in str_vas:
    pat = struct.pack("<I", sva); off = 0
    while True:
        i = data.find(pat, off)
        if i < 0: break
        execute = struct.unpack_from("<I", data, i + 0x18)[0]
        opcode = struct.unpack_from("<I", data, i + 0x08)[0]
        if in_text(execute) and 0x1000 <= (opcode & 0xFFFF) <= 0x2000:
            print("  CI@0x%08X opcode=0x%04X execute=0x%08X" % (base+i, opcode & 0xFFFF, execute))
            execs.append(execute)
        off = i + 1

md = Cs(CS_ARCH_X86, CS_MODE_32)
for ex in execs:
    print("\n=== disasm execute 0x%08X ===" % ex)
    code = data[ex-base: ex-base+220]
    for ins in md.disasm(code, ex):
        mark = "   <<< CALL" if ins.mnemonic == "call" else ""
        print("  0x%08X  %-9s %s%s" % (ins.address, ins.mnemonic, ins.op_str, mark))
        if ins.mnemonic == "ret": break

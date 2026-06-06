import pefile, struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Fallout 3\Fallout3.exe"
pe = pefile.PE(EXE, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = pe.get_memory_mapped_image()   # indexed by RVA
print("ImageBase 0x%08X  size 0x%X" % (base, len(data)))

# section ranges (RVA)
secs = []
for s in pe.sections:
    name = s.Name.rstrip(b"\x00").decode("latin1")
    secs.append((name, s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize))
    print("  %-8s RVA 0x%08X .. 0x%08X" % (name, s.VirtualAddress,
                                            s.VirtualAddress + s.Misc_VirtualSize))

def sec_of(rva):
    for n, a, b in secs:
        if a <= rva < b:
            return n
    return None

text = next((s for s in secs if s[0] == ".text"), None)
def in_text(va):
    rva = va - base
    return text and text[1] <= rva < text[2]

# 1) find "Activate\0" exact strings
needle = b"Activate\x00"
str_vas = []
off = 0
while True:
    i = data.find(needle, off)
    if i < 0: break
    # require it's a standalone string (preceded by \0) to avoid substrings
    if i == 0 or data[i-1] == 0:
        str_vas.append(base + i)
    off = i + 1
print("Activate string VAs:", [hex(v) for v in str_vas])

# 2) scan for dword pointers to those VAs (CommandInfo.longName candidates)
cands = []
for sva in str_vas:
    pat = struct.pack("<I", sva)
    off = 0
    while True:
        i = data.find(pat, off)
        if i < 0: break
        ptr_va = base + i
        # interpret i as CommandInfo.longName -> read execute @ +0x18
        ci = i
        def rd(o):
            return struct.unpack_from("<I", data, ci + o)[0]
        longName = rd(0x00); shortName = rd(0x04); opcode = rd(0x08)
        execute = rd(0x18); parse = rd(0x1C)
        if in_text(execute) and 0x1000 <= (opcode & 0xFFFF) <= 0x2000:
            cands.append((ptr_va, opcode, execute, shortName))
        off = i + 1

print("\nCommandInfo candidates (longName@VA, opcode, execute, shortName):")
for ptr_va, opcode, execute, shortName in cands:
    print("  CI@0x%08X opcode=0x%04X execute=0x%08X shortName=0x%08X"
          % (ptr_va, opcode & 0xFFFF, execute, shortName))

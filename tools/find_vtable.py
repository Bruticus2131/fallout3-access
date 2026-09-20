import pefile, struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

import os
EXE = os.environ.get("F3_EXE", r"D:\SteamLibrary\steamapps\common\Fallout 3 goty\Fallout3.exe")
pe = pefile.PE(EXE, fast_load=True)
base = pe.OPTIONAL_HEADER.ImageBase
data = pe.get_memory_mapped_image()
secs = [(s.Name.rstrip(b"\x00").decode("latin1"), base + s.VirtualAddress,
         base + s.VirtualAddress + s.Misc_VirtualSize) for s in pe.sections]
text = next(s for s in secs if s[0] == ".text")
def in_text(va): return text[1] <= va < text[2]
def rd(va, n=4):
    off = va - base
    return data[off:off+n]
def rdu32(va): return struct.unpack_from("<I", data, va - base)[0]

def find_str(needle):
    out=[]
    off=0
    b=needle.encode()
    while True:
        i=data.find(b, off)
        if i<0: break
        out.append(base+i); off=i+1
    return out

def find_dword_refs(value):
    pat=struct.pack("<I", value); out=[]; off=0
    while True:
        i=data.find(pat, off)
        if i<0: break
        out.append(base+i); off=i+1
    return out

md = Cs(CS_ARCH_X86, CS_MODE_32)

def dump_vtable(name):
    print("\n==================== %s ====================" % name)
    # MSVC type descriptor name: ".?AV<name>@@"
    tdname = ".?AV%s@@" % name
    addrs = find_str(tdname)
    if not addrs:
        print("  type descriptor string not found:", tdname); return
    for nameaddr in addrs:
        td = nameaddr - 8          # TypeDescriptor starts 8 bytes before the name
        print("  TD @ 0x%08X (name @ 0x%08X)" % (td, nameaddr))
        # Complete Object Locators point to TD at COL+0x0C
        for ref in find_dword_refs(td):
            col = ref - 0x0C
            sig = rdu32(col)
            if sig not in (0,1): continue
            # vtable: a dword == col exists at [vtable-4]
            for vref in find_dword_refs(col):
                vt = vref + 4
                # dump consecutive code pointers
                fns=[]
                a=vt
                for _ in range(80):
                    p=rdu32(a)
                    if not in_text(p): break
                    fns.append(p); a+=4
                if len(fns)>=2:
                    print("    vtable @ 0x%08X  (%d methods)" % (vt, len(fns)))
                    for i,f in enumerate(fns[:32]):
                        print("      [%2d] 0x%08X" % (i, f))

for n in (sys.argv[1:] or ["ActorMover","PlayerMover","PathingSolution","CharacterMover"]):
    dump_vtable(n)

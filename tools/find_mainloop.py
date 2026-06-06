import pefile, struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\Fallout 3\Fallout3.exe"
pe = pefile.PE(EXE)
base = pe.OPTIONAL_HEADER.ImageBase
data = pe.get_memory_mapped_image()

# Find IAT slots for the message-pump APIs.
targets = {}
for entry in pe.DIRECTORY_ENTRY_IMPORT:
    for imp in entry.imports:
        if imp.name and imp.name.decode("latin1") in (
                "PeekMessageA", "DispatchMessageA", "TranslateMessage",
                "PeekMessageW", "DispatchMessageW"):
            targets[imp.name.decode("latin1")] = imp.address  # VA of IAT slot
print("IAT slots:", {k: hex(v) for k, v in targets.items()})

# Search .text for `call dword ptr [IATslot]` (FF 15 <abs ptr>).
md = Cs(CS_ARCH_X86, CS_MODE_32)
for name, slot in targets.items():
    pat = b"\xFF\x15" + struct.pack("<I", slot)
    off = 0
    hits = []
    while True:
        i = data.find(pat, off)
        if i < 0: break
        hits.append(base + i)
        off = i + 1
    print("%s called at:" % name, [hex(h) for h in hits])

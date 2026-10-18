"""
Build the table of items worth a mention for Fallout 3 Access: which loose
items exploration announces on the way.

Exploration used to name every item near the route, and a vault is full of
coffee mugs and tin plates: "Po drodze: Kubek na kawę" every few seconds. So
items of the cheap kinds - misc things, aid (food, drinks, chems), armour,
clothing, ingredients - are announced only when they are worth something
(value >= MIN_VALUE caps) or carry a script (bobbleheads have value 0 and a
script). Weapons, ammo, keys, books and notes are always worth a mention and
are not listed.

Ids are written like the door table's (tools/generate_world_doors.py): top
byte = the plugin's index in the file's module table (0 = Fallout3.esm).

Output (little-endian), default data/roadnet/items.dat:
    char[4] 'F3AV', u32 version (1), u32 nModules, u32 nEntries
    nModules x char name[64]
    nEntries x u32 base form id (worth a mention), sorted

Usage:
    python generate_item_values.py [--data DIR] [--output PATH]
"""

import argparse
import os
import struct
import zlib

DEFAULT_DATA = r"D:\SteamLibrary\steamapps\common\Fallout 3 goty\Data"
DEFAULT_PLUGINS = ['Fallout3.esm', 'Anchorage.esm', 'ThePitt.esm', 'BrokenSteel.esm',
                   'PointLookout.esm', 'Zeta.esm']
MIN_VALUE = 10
# record type -> (subrecord holding the value, offset of the int32 value)
VALUE_AT = {b'MISC': (b'DATA', 0), b'ALCH': (b'ENIT', 0), b'ARMO': (b'DATA', 0),
            b'CLOT': (b'DATA', 0), b'INGR': (b'ENIT', 0)}


def subrecords(buf):
    i, out = 0, []
    while i + 6 <= len(buf):
        typ = buf[i:i + 4]
        size = struct.unpack_from('<H', buf, i + 4)[0]
        if typ == b'XXXX':
            big = struct.unpack_from('<I', buf, i + 6)[0]
            i += 10
            typ = buf[i:i + 4]
            out.append((typ, buf[i + 6:i + 6 + big]))
            i += 6 + big
            continue
        out.append((typ, buf[i + 6:i + 6 + size]))
        i += 6 + size
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    ap.add_argument('--data', default=DEFAULT_DATA)
    ap.add_argument('--output', default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..', 'data', 'roadnet', 'items.dat'))
    args = ap.parse_args()
    modules = [p for p in DEFAULT_PLUGINS if os.path.exists(os.path.join(args.data, p))]
    lower = [m.lower() for m in modules]
    worth = {}                     # id -> bool (the last plugin's version wins)
    for mi, m in enumerate(modules):
        data = open(os.path.join(args.data, m), 'rb').read()
        hdr = struct.unpack_from('<I', data, 4)[0]
        masters = [sd.split(b'\0', 1)[0].decode('ascii', 'replace')
                   for st, sd in subrecords(data[24:24 + hdr]) if st == b'MAST']
        own = len(masters)

        def canon(fid):
            h = fid >> 24
            if h >= own:
                return (mi << 24) | (fid & 0xFFFFFF)
            n = masters[h].lower()
            return (lower.index(n) << 24) | (fid & 0xFFFFFF) if n in lower else None

        off = 24 + hdr
        while off < len(data):
            typ = data[off:off + 4]
            size = struct.unpack_from('<I', data, off + 4)[0]
            if typ != b'GRUP':
                off += 24 + size
                continue
            label = data[off + 8:off + 12]
            if label in VALUE_AT:
                sub, at = VALUE_AT[label]
                o, end = off + 24, off + size
                while o < end:
                    flags, raw = struct.unpack_from('<II', data, o + 8)
                    rsize = struct.unpack_from('<I', data, o + 4)[0]
                    body = data[o + 24:o + 24 + rsize]
                    o += 24 + rsize
                    fid = canon(raw)
                    if fid is None or flags & 0x20:
                        continue
                    if flags & 0x40000:
                        try:
                            body = zlib.decompress(body[4:])
                        except zlib.error:
                            continue
                    ss = subrecords(body)
                    value, script = 0, False
                    for t, v in ss:
                        if t == sub and len(v) >= at + 4:
                            value = struct.unpack_from('<i', v, at)[0]
                        elif t == b'SCRI':
                            script = True
                    worth[fid] = value >= MIN_VALUE or (label == b'MISC' and script)
            off += size
    ids = sorted(f for f, w in worth.items() if w)
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, 'wb') as f:
        f.write(b'F3AV')
        f.write(struct.pack('<III', 1, len(modules), len(ids)))
        for m in modules:
            mb = m.encode('ascii')[:63]
            f.write(mb + b'\0' * (64 - len(mb)))
        f.write(struct.pack('<%dI' % len(ids), *ids))
    print('%d of %d cheap-kind items are worth a mention; wrote %s (%d bytes)' % (
        len(ids), len(worth), os.path.abspath(args.output), os.path.getsize(args.output)))


if __name__ == '__main__':
    main()

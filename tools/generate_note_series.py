"""
Build the note-series table for Fallout 3 Access: holotapes and notes that
come in a series and lie about the world, from Fallout3.esm and the DLCs.

Some quests are no more than a set of holotapes to find: the Keller family
transcripts are five tapes scattered over the whole Wasteland, and nothing in
the Pip-Boy points at them. A sighted player stumbles on them, or reads a
guide. Once the player has one tape of a series, the mod's "quest objects"
category lists the ones still to find, with the way there.

A SERIES is a group of notes whose editor ids differ only in the number at the
end (KellerHolotape1 ... KellerHolotape5), at least two of them lying in the
world. Each note placed in the world is one entry: the note, its reference,
the space it lies in (an interior cell, or a worldspace) and its position.
References that start the game switched off are left out, as are references a
later plugin deleted.

Ids are written like the door table's (tools/generate_world_doors.py): the top
byte is the plugin's index in the file's module table (0 = Fallout3.esm); the
mod puts in the real load-order byte and leaves out unloaded DLCs.

Output (little-endian), default data/roadnet/notes.dat:
    char[4] 'F3AN', u32 version (1), u32 nModules, u32 nEntries
    nModules x char name[64]
    nEntries x { u32 note, u32 ref, u32 space, f32 x, f32 y, f32 z,
                 u16 series, u16 order }          sorted by series, order

Usage:
    python generate_note_series.py [--data DIR] [--output PATH] [--verbose]
"""

import argparse
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from generate_world_doors import subrecords, body_of, masters_of, text   # noqa: E402

DEFAULT_DATA = r"D:\SteamLibrary\steamapps\common\Fallout 3 goty\Data"
DEFAULT_PLUGINS = ['Fallout3.esm', 'Anchorage.esm', 'ThePitt.esm', 'BrokenSteel.esm',
                   'PointLookout.esm', 'Zeta.esm']
MAGIC = b'F3AN'
VERSION = 1
FLAG_DELETED = 0x20
FLAG_DISABLED = 0x800


def read(path, module, modules, notes, placed):
    data = open(path, 'rb').read()
    masters = masters_of(data)
    own = len(masters)
    lower = [m.lower() for m in modules]

    def canon(fid):
        h = fid >> 24
        if h >= own:
            m = module
        else:
            name = masters[h].lower()
            if name not in lower:
                return None
            m = lower.index(name)
        return (m << 24) | (fid & 0xFFFFFF)

    def walk(off, end, world, cell, interior):
        while off < end:
            typ = data[off:off + 4]
            size = struct.unpack_from('<I', data, off + 4)[0]
            if typ == b'GRUP':
                label = struct.unpack_from('<I', data, off + 8)[0]
                gtype = struct.unpack_from('<I', data, off + 12)[0]
                w, c, inter = world, cell, interior
                if gtype == 1:                 # world children
                    w, inter = canon(label), False
                elif gtype in (2, 3):          # interior cell block / sub-block
                    w, inter = 0, True
                elif gtype in (6, 8, 9, 10):   # cell children
                    c = canon(label)
                walk(off + 24, off + size, w, c, inter)
                off += size
                continue
            flags, raw = struct.unpack_from('<II', data, off + 8)
            fid = canon(raw)
            if fid is not None and typ in (b'NOTE', b'REFR'):
                if typ == b'REFR' and flags & FLAG_DELETED:
                    placed.pop(fid, None)      # a later plugin took it out
                else:
                    body = body_of(data, off, size, flags)
                    if body is not None:
                        d = {}
                        for st, sd in subrecords(body):
                            d.setdefault(st, sd)
                        if typ == b'NOTE':
                            notes[fid] = text(d.get(b'EDID', b''))
                        elif b'NAME' in d and b'DATA' in d and len(d[b'DATA']) >= 12:
                            base = canon(struct.unpack_from('<I', d[b'NAME'])[0])
                            space = cell if interior else world
                            if base and space and not flags & FLAG_DISABLED:
                                x, y, z = struct.unpack_from('<3f', d[b'DATA'])
                                placed[fid] = (base, space, x, y, z)
                            else:
                                placed.pop(fid, None)
            off += 24 + size

    hdr = struct.unpack_from('<I', data, 4)[0]
    walk(24 + hdr, len(data), 0, 0, False)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    ap.add_argument('--data', default=DEFAULT_DATA)
    ap.add_argument('--output', default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..', 'data', 'roadnet', 'notes.dat'))
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()
    if args.verbose:
        sys.stdout.reconfigure(encoding='utf-8')
    modules = [p for p in DEFAULT_PLUGINS if os.path.exists(os.path.join(args.data, p))]
    if not modules or modules[0].lower() != 'fallout3.esm':
        raise SystemExit('Fallout3.esm must be there, first')
    notes, placed = {}, {}
    for i, m in enumerate(modules):
        read(os.path.join(args.data, m), i, modules, notes, placed)
        print('%-18s read' % m)

    # Notes lying in the world, by editor-id stem.
    stems = {}
    for ref, (base, space, x, y, z) in placed.items():
        edid = notes.get(base)
        if edid is None:
            continue
        mo = re.match(r'^(.*?)(\d+)$', edid)
        if not mo:
            continue
        stems.setdefault(mo.group(1).lower(), []).append((int(mo.group(2)), base, ref, space, x, y, z, edid))
    series = sorted((s, v) for s, v in stems.items() if len({b for _, b, *_ in v}) >= 2)

    rows = []
    for si, (stem, members) in enumerate(series):
        for order, base, ref, space, x, y, z, edid in sorted(members):
            rows.append((base, ref, space, x, y, z, si, order))
        if args.verbose:
            print('%-34s %s' % (stem, ', '.join(e for *_, e in sorted(members))))

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, 'wb') as f:
        f.write(MAGIC + struct.pack('<III', VERSION, len(modules), len(rows)))
        for m in modules:
            f.write(m.encode('ascii')[:63].ljust(64, b'\0'))
        for base, ref, space, x, y, z, si, order in rows:
            f.write(struct.pack('<III3fHH', base, ref, space, x, y, z, si, order))
    print('%d series, %d notes in the world -> %s (%d bytes)' %
          (len(series), len(rows), args.output, os.path.getsize(args.output)))


if __name__ == '__main__':
    main()

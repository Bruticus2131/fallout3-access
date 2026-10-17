"""
Build the world-door graph for Fallout 3 Access from Fallout3.esm and the DLCs.

Auto-walk crosses from one place to another through load doors (a door that
teleports the player to another cell or worldspace). Near doors are found in
memory, but a target several places away — Megaton from the Vault 101 exit,
the bomb inside it — needs to know which door, possibly kilometres away,
leads towards it. That is a property of the game data, so it is worked out
here once:

  * every SPACE: a worldspace (its exterior shares one coordinate frame) or an
    interior cell, with its display name;
  * every LOAD DOOR: the space it stands in, its position, and the door it
    leads to (and so the space on the far side);
  * every PERSISTENT reference and the space it lives in, because a quest
    target in a place that is not loaded has no cell in memory to ask;
  * the DLC TRAVEL points that are not doors: the handcart to the Pitt, the
    Duchess Gambit's berth to Point Lookout, the presidential metro console.
    A script moves the player when they are activated, so they are listed by
    hand (TRAVEL below) from the scripts' MoveTo markers.

Doors that start the game switched off are left out: initially disabled, or
following an enable parent that leaves them off (XESP, the "opposite" bit
against an enabled parent). Megaton's quest escape hatch is one: it stands
1700 units nearer the Vault 101 road than the real gate, so the walk went to
a door that is not there.

DLCs: a form id's top byte is the plugin's place in the load order, which is
only known in the game. Here every id is written with the top byte set to
the plugin's index in the file's MODULE table (0 = Fallout3.esm); the mod
puts in the real load-order byte and leaves out what belongs to a DLC that
is not loaded. A DLC can change a Fallout3.esm door (move it, link it
elsewhere, delete it): each such change is one more VERSION of the door,
tagged with the plugin it comes from, and the mod uses the version of the
last plugin it has loaded.

Output (little-endian), default data/roadnet/doors.dat:
    char[4] 'F3AD', u32 version (2), u32 nModules, u32 nSpaces, u32 nDoors, u32 nRefs
    nModules x char name[64]                      (load order; [0] = Fallout3.esm)
    nSpaces x { u32 id, u32 flags (1 = worldspace), char name[64] UTF-8 }
    nDoors  x { u32 id, u32 space, f32 x, f32 y, f32 z, u32 dest (door id, 0 =
               none), u8 module (the version's plugin), u8 kind, u16 0 }
               sorted by id, then module; kind 0 = load door, 1 = handcart,
               2 = boat, 3 = metro console, 0xFE = deleted by that plugin,
               0xFF = where a travel point lands the player
    nRefs   x { u32 id, u32 space }               sorted by id

Usage:
    python generate_world_doors.py [--data DIR] [--plugin X.esm ...] [--output PATH] [--verbose]
"""

import argparse
import os
import struct
import zlib

DEFAULT_DATA = r"D:\SteamLibrary\steamapps\common\Fallout 3 goty\Data"
# Fallout 3 GOTY's own load order.
DEFAULT_PLUGINS = ['Fallout3.esm', 'Anchorage.esm', 'ThePitt.esm', 'BrokenSteel.esm',
                   'PointLookout.esm', 'Zeta.esm']
MAGIC = b'F3AD'
VERSION = 2
FLAG_DELETED = 0x20
FLAG_PERSISTENT = 0x400
FLAG_DISABLED = 0x800        # "initially disabled"
FLAG_COMPRESSED = 0x40000
NAME_LEN = 64
KIND_DOOR, KIND_HANDCART, KIND_BOAT, KIND_METRO = 0, 1, 2, 3
KIND_DELETED, KIND_LANDING = 0xFE, 0xFF

# Travel points that are not doors: (plugin, the activator (an editor id, or
# the plugin's own form id when it has none), the marker its script moves the
# player to, kind). From the scripts' "player.moveto" lines.
TRAVEL = [
    # DLC01TunnelHandcartDCSCRIPT / ...PittSCRIPT: the handcart in the tunnel
    # (allowed with Wernher's quest, then after "Unsafe Working Conditions").
    ('ThePitt.esm', 0x00A75C, 'DLC01ThePittPlayerTravelMarker', KIND_HANDCART),
    ('ThePitt.esm', 0x00A60B, 'DLC01DCPlayerTravelMarker', KIND_HANDCART),
    # DLC04*DuchessTravelActivatorSCRIPT: the berth, with Tobar's ticket.
    ('PointLookout.esm', 'DLC04CapitalDuchessTravelActivatorREF',
     'DLC04LookoutLandingMarker02REF', KIND_BOAT),
    ('PointLookout.esm', 'DLC04LookoutDuchessTravelActivatorREF',
     'DLC04CapitalLandingMarkerREF', KIND_BOAT),
    # DLC03PresMetroRide01/02AnimSCRIPT: the train ride ends at the other end.
    ('BrokenSteel.esm', 'DLC03MetroSwitch01REF', 'DLC03PresMetroRideEnd02REF', KIND_METRO),
    ('BrokenSteel.esm', 'DLC03MetroSwitch02REF', 'DLC03PresMetroRideEnd01REF', KIND_METRO),
]


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


def body_of(data, off, size, flags):
    body = data[off + 24:off + 24 + size]
    if flags & FLAG_COMPRESSED:
        try:
            body = zlib.decompress(body[4:])
        except zlib.error:
            return None
    return body


def text(b):
    return b.split(b'\0', 1)[0].decode('cp1250', 'replace')


def masters_of(data):
    hdr = struct.unpack_from('<I', data, 4)[0]
    return [text(sd) for st, sd in subrecords(data[24:24 + hdr]) if st == b'MAST']


class World:
    """Everything read so far, ids in the file's module numbering."""

    def __init__(self, modules):
        self.modules = modules
        self.spaces = {}        # id -> [flags, name]
        self.doors = {}         # id -> [(module, space, x, y, z, dest, kind)]
        self.refs = {}          # id -> space
        self.enable = {}        # id -> (initially disabled, parent id, opposite)
        self.edids = {}         # (module, lower-case editor id) -> id
        self.placed = {}        # id -> (space, x, y, z), DLC references only

    def read(self, path, module):
        data = open(path, 'rb').read()
        masters = masters_of(data)
        own = len(masters)
        lower = [m.lower() for m in self.modules]

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
                if fid is None:
                    off += 24 + size
                    continue
                if typ in (b'WRLD', b'CELL'):
                    body = body_of(data, off, size, flags)
                    name = edid = ''
                    if body is not None:
                        for st, sd in subrecords(body):
                            if st == b'FULL':
                                name = text(sd)
                            elif st == b'EDID':
                                edid = text(sd)
                    if typ == b'WRLD':
                        self.spaces[fid] = [1, name or edid]
                    elif interior:
                        self.spaces[fid] = [0, name or edid]
                elif typ in (b'REFR', b'ACHR', b'ACRE'):
                    space = cell if interior else world
                    if space:
                        self.reference(typ, fid, flags, data, off, size, space, module, canon)
                off += 24 + size

        hdr = struct.unpack_from('<I', data, 4)[0]
        walk(24 + hdr, len(data), 0, 0, False)

    def reference(self, typ, fid, flags, data, off, size, space, module, canon):
        if flags & FLAG_DELETED:
            if fid in self.doors:
                self.doors[fid].append((module, space, 0.0, 0.0, 0.0, 0, KIND_DELETED))
            self.refs.pop(fid, None)
            return
        if typ == b'REFR':
            body = body_of(data, off, size, flags)
            if body is not None:
                dest = pos = None
                parent, opposite = 0, False
                for st, sd in subrecords(body):
                    if st == b'XTEL' and len(sd) >= 4:
                        dest = canon(struct.unpack_from('<I', sd, 0)[0]) or 0
                    elif st == b'DATA' and len(sd) >= 12:
                        pos = struct.unpack_from('<3f', sd, 0)
                    elif st == b'XESP' and len(sd) >= 5:
                        parent = canon(struct.unpack_from('<I', sd, 0)[0]) or 0
                        opposite = (sd[4] & 1) != 0
                    elif st == b'EDID' and module > 0:
                        self.edids[(module, text(sd).lower())] = fid
                self.enable[fid] = ((flags & FLAG_DISABLED) != 0, parent, opposite)
                if pos is not None and module > 0:
                    self.placed[fid] = (space,) + tuple(pos)
                if typ == b'REFR' and dest is not None and pos is not None:
                    self.doors.setdefault(fid, []).append(
                        (module, space, pos[0], pos[1], pos[2], dest, KIND_DOOR))
                elif fid in self.doors:
                    # A plugin took the teleport off a door: no longer a load door there.
                    self.doors[fid].append((module, space, 0.0, 0.0, 0.0, 0, KIND_DELETED))
        if flags & FLAG_PERSISTENT:
            self.refs[fid] = space

    # Is a reference switched on when the game starts? An enable parent
    # decides for its children (the same state, or the opposite with the
    # bit set); a reference we know nothing about counts as on.
    def enabled(self, fid, depth=0):
        info = self.enable.get(fid)
        if info is None or depth > 8:
            return True
        disabled, parent, opposite = info
        if parent:
            on = self.enabled(parent, depth + 1)
            return (not on) if opposite else on
        return not disabled


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    ap.add_argument('--data', default=DEFAULT_DATA, help="the game's Data folder")
    ap.add_argument('--plugin', action='append',
                    help='plugins in load order (default: Fallout3.esm and the five DLCs)')
    ap.add_argument('--output', default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..', 'data', 'roadnet', 'doors.dat'))
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    modules = [p for p in (args.plugin or DEFAULT_PLUGINS)
               if os.path.exists(os.path.join(args.data, p))]
    if not modules or modules[0].lower() != 'fallout3.esm':
        raise SystemExit('Fallout3.esm must be there, first')
    w = World(modules)
    for i, m in enumerate(modules):
        before = (len(w.spaces), len(w.doors))
        w.read(os.path.join(args.data, m), i)
        print('%-18s +%d places, +%d doors' % (m, len(w.spaces) - before[0],
                                                  len(w.doors) - before[1]))

    # Travel points: the activator as a "door" leading to its landing marker.
    for plugin, source, marker, kind in TRAVEL:
        if plugin not in modules:
            continue
        mi = modules.index(plugin)
        src = ((mi << 24) | source) if isinstance(source, int) else \
            w.edids.get((mi, source.lower()))
        dst = w.edids.get((mi, marker.lower()))
        if src not in w.placed or dst not in w.placed:
            print('  travel point %s -> %s: not found, left out' % (source, marker))
            continue
        s, d = w.placed[src], w.placed[dst]
        w.doors.setdefault(dst, []).append((mi, d[0], d[1], d[2], d[3], 0, KIND_LANDING))
        w.doors.setdefault(src, []).append((mi, s[0], s[1], s[2], s[3], dst, kind))
        if args.verbose:
            print('  travel %08X in %s -> %08X in %s' % (src, w.spaces.get(s[0], ['', '?'])[1],
                                                      dst, w.spaces.get(d[0], ['', '?'])[1]))

    off_doors = [fid for fid, vs in w.doors.items()
                 if vs[-1][6] == KIND_DOOR and not w.enabled(fid)]
    if args.verbose:
        for fid in off_doors:
            print('  switched off at the start, left out: door %08X' % fid)
    for fid in off_doors:
        del w.doors[fid]
    rows = []
    for fid in sorted(w.doors):
        for v in w.doors[fid]:
            if v[6] != KIND_DELETED and v[1] not in w.spaces:
                continue                     # in a place we do not know
            rows.append((fid,) + v)
    ref_list = sorted((f, s) for f, s in w.refs.items() if s in w.spaces)
    ids = set(w.doors)
    unknown = sum(1 for r in rows if r[7] == KIND_DOOR and r[6] not in ids)
    print('places: %d (%d worldspaces), door versions: %d (%d with an unknown far side, '
          '%d switched off and left out), persistent refs: %d' % (
              len(w.spaces), sum(1 for s in w.spaces.values() if s[0]),
              len(rows), unknown, len(off_doors), len(ref_list)))

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, 'wb') as f:
        f.write(MAGIC)
        f.write(struct.pack('<IIIII', VERSION, len(modules), len(w.spaces), len(rows),
                            len(ref_list)))
        for m in modules:
            mb = m.encode('ascii')[:NAME_LEN - 1]
            f.write(mb + b'\0' * (NAME_LEN - len(mb)))
        for s in sorted(w.spaces):
            fl, name = w.spaces[s]
            nb = name.encode('utf-8')[:NAME_LEN - 1]
            while nb:
                try:
                    nb.decode('utf-8')
                    break
                except UnicodeDecodeError:
                    nb = nb[:-1]             # a character cut in half
            f.write(struct.pack('<II', s, fl))
            f.write(nb + b'\0' * (NAME_LEN - len(nb)))
        for fid, module, space, x, y, z, dest, kind in rows:
            f.write(struct.pack('<II3fIBBH', fid, space, x, y, z, dest, module, kind, 0))
        for fid, s in ref_list:
            f.write(struct.pack('<II', fid, s))
    print('wrote %s (%d bytes)' % (os.path.abspath(args.output), os.path.getsize(args.output)))

    if args.verbose:
        by = {}
        for r in rows:
            by[r[2]] = by.get(r[2], 0) + 1
        for s, n in sorted(by.items(), key=lambda kv: -kv[1])[:15]:
            print('  %08X %-40s %d doors' % (s, w.spaces[s][1], n))


if __name__ == '__main__':
    main()

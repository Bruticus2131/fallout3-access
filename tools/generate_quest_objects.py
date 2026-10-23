"""
Build the quest-object table for Fallout 3 Access: the things in the world a
quest waits for the player to use, from Fallout3.esm and the DLCs.

The scanner's "quest objects" category lists, near the player, only what
moves a running quest on:

  * ACTIVATORS, TERMINALS, DOORS, FURNITURE, CONTAINERS and ITEMS whose own
    script advances a quest when they are used (SetStage in an OnActivate,
    OnAdd or OnEquip block; for a terminal, in a menu entry's result script):
    the vault door's control panel and the reactor switches in "Escape!",
    the Overseer's terminal, the vault exit;
  * TRIGGER spots: invisible boxes whose OnTriggerEnter/OnTrigger script
    advances a quest when the player walks in;
  * SEATS: furniture inside a trigger that waits for the player to sit down
    (player.GetSitting) - the free desks in the G.O.A.T. classroom; seats an
    NPC is sent to by a package are left out;
  * OBJECTIVE TARGETS that are not people: the reference a quest objective's
    compass marker points at (an item to fetch, a place to reach).

Each entry carries the stage its script sets (shown only while the quest is
running and has not reached it) or the objective it belongs to (shown only
while that objective is on the list and not done).

Ids are written like the door table's (tools/generate_world_doors.py): the
top byte is the plugin's index in the file's module table (0 = Fallout3.esm);
the mod puts in the real load-order byte and leaves out unloaded DLCs.

Output (little-endian), default data/roadnet/questobjs.dat:
    char[4] 'F3AQ', u32 version (1), u32 nModules, u32 nEntries
    nModules x char name[64]
    nEntries x { u32 ref, u32 quest, u16 stage (0 = none), i16 objective
                 (-1 = none), u8 kind, u8 pad[3] }                sorted by ref
    kind: 1 activator, 2 terminal, 3 door, 4 trigger spot, 5 seat,
          6 marker / place, 7 item, 8 furniture, 9 container

Usage:
    python generate_quest_objects.py [--data DIR] [--output PATH] [--verbose]
"""

import argparse
import math
import os
import re
import struct
import sys
import zlib

DEFAULT_DATA = r"D:\SteamLibrary\steamapps\common\Fallout 3 goty\Data"
DEFAULT_PLUGINS = ['Fallout3.esm', 'Anchorage.esm', 'ThePitt.esm', 'BrokenSteel.esm',
                   'PointLookout.esm', 'Zeta.esm']
MAGIC = b'F3AQ'
VERSION = 1
FLAG_DELETED = 0x20
FLAG_COMPRESSED = 0x40000
K_ACTIVATOR, K_TERMINAL, K_DOOR, K_TRIGGER, K_SEAT, K_MARKER, K_ITEM, K_FURN, K_CONT = range(1, 10)
ITEM_TYPES = {b'MISC', b'KEYM', b'BOOK', b'NOTE', b'ALCH', b'WEAP', b'ARMO', b'AMMO'}
KIND_OF_BASE = {b'ACTI': K_ACTIVATOR, b'TACT': K_ACTIVATOR, b'TERM': K_TERMINAL, b'DOOR': K_DOOR,
                b'FURN': K_FURN, b'CONT': K_CONT, b'STAT': K_MARKER, b'LIGH': K_ACTIVATOR}
INTERACT_BLOCKS = {'onactivate', 'onadd', 'onequip'}
TRIGGER_BLOCKS = {'ontriggerenter', 'ontrigger'}
SETSTAGE = re.compile(r'(?i)\bsetstage\s+(\w+)\s+(\d+)')
# A quest moved on by its objective instead of a stage: the Wasteland Survival
# Guide's pantry and pharmacy supplies in the Super-Duper Mart complete
# "find food" / "find medicine" with SetObjectiveCompleted MS03 10 / 12.
SETOBJECTIVE = re.compile(r'(?i)\bsetobjectivecompleted\s+(\w+)\s+(\d+)')
BLOCK = re.compile(r'(?ims)^\s*begin\s+(\w+)(.*?)^\s*end\b')


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


def text(b):
    return b.split(b'\0', 1)[0].decode('cp1250', 'replace')


def strip_comments(src):
    return '\n'.join(line.split(';', 1)[0] for line in src.splitlines())


class Data:
    """Everything read, ids in the module numbering."""

    def __init__(self, modules):
        self.modules = modules
        self.quest_edid = {}     # lower EDID -> quest id
        self.objectives = []     # (quest, objective, target ref)
        self.scripts = {}        # script id -> source text
        self.base = {}           # base id -> (type, script id, edid lower)
        self.term_sets = {}      # terminal base id -> [(quest edid, stage)]
        self.refs = {}           # ref id -> (base id, x, y, z, rotz, xprm or None, module)
        self.pack_targets = set()

    def read(self, path, module):
        data = open(path, 'rb').read()
        hdr = struct.unpack_from('<I', data, 4)[0]
        masters = [text(sd) for st, sd in subrecords(data[24:24 + hdr]) if st == b'MAST']
        own = len(masters)
        lower = [m.lower() for m in self.modules]

        def canon(fid):
            h = fid >> 24
            if h >= own:
                m = module
            else:
                n = masters[h].lower()
                if n not in lower:
                    return None
                m = lower.index(n)
            return (m << 24) | (fid & 0xFFFFFF)

        def walk(off, end):
            while off < end:
                typ = data[off:off + 4]
                size = struct.unpack_from('<I', data, off + 4)[0]
                if typ == b'GRUP':
                    walk(off + 24, off + size)
                    off += size
                    continue
                flags, raw = struct.unpack_from('<II', data, off + 8)
                off2 = off + 24 + size
                fid = canon(raw)
                if fid is None or flags & FLAG_DELETED:
                    off = off2
                    continue
                body = data[off + 24:off2]
                if flags & FLAG_COMPRESSED:
                    try:
                        body = zlib.decompress(body[4:])
                    except zlib.error:
                        off = off2
                        continue
                self.record(typ, fid, subrecords(body), canon, module)
                off = off2

        walk(24 + hdr, len(data))

    def record(self, typ, fid, ss, canon, module):
        get = lambda t: next((v for s, v in ss if s == t), None)
        if typ == b'QUST':
            e = get(b'EDID')
            if e:
                self.quest_edid[text(e).lower()] = fid
            obj = None
            for s, v in ss:
                if s == b'QOBJ':
                    obj = struct.unpack('<i', v[:4])[0]
                elif s == b'QSTA' and obj is not None and len(v) >= 4:
                    t = canon(struct.unpack('<I', v[:4])[0])
                    if t:
                        self.objectives.append((fid, obj, t))
        elif typ == b'SCPT':
            src = get(b'SCTX')
            if src:
                self.scripts[fid] = strip_comments(src.decode('cp1250', 'replace'))
        elif typ in (b'REFR',):
            n, d = get(b'NAME'), get(b'DATA')
            if n and d and len(d) >= 24:
                b = canon(struct.unpack('<I', n[:4])[0])
                x, y, z, rx, ry, rz = struct.unpack('<6f', d[:24])
                prm = get(b'XPRM')
                box = struct.unpack('<3f', prm[:12]) if prm and len(prm) >= 12 else None
                self.refs[fid] = (b, x, y, z, rz, box, module)
        elif typ == b'PACK':
            for s, v in ss:
                if s in (b'PLDT', b'PTDT') and len(v) >= 8 and struct.unpack_from('<i', v, 0)[0] == 0:
                    t = canon(struct.unpack_from('<I', v, 4)[0])
                    if t:
                        self.pack_targets.add(t)
        elif typ in KIND_OF_BASE or typ in ITEM_TYPES or typ in (b'NPC_', b'CREA'):
            sc = get(b'SCRI')
            e = get(b'EDID')
            self.base[fid] = (typ, canon(struct.unpack('<I', sc[:4])[0]) if sc else None,
                              text(e).lower() if e else '')
            if typ == b'TERM':
                sets = []
                for s, v in ss:
                    if s == b'SCTX':
                        sets += SETSTAGE.findall(strip_comments(v.decode('cp1250', 'replace')))
                if sets:
                    self.term_sets[fid] = sets


def inside(box_ref, p, pad=48.0):
    """Is point p inside the trigger box (half extents, turned by rotZ)?"""
    _, x, y, z, rz, box, _ = box_ref
    hx, hy, hz = box
    dx, dy = p[0] - x, p[1] - y
    c, s = math.cos(-rz), math.sin(-rz)
    lx, ly = dx * c - dy * s, dx * s + dy * c
    return abs(lx) <= hx + pad and abs(ly) <= hy + pad and abs(p[2] - z) <= hz + 120.0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    ap.add_argument('--data', default=DEFAULT_DATA)
    ap.add_argument('--plugin', action='append')
    ap.add_argument('--output', default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..', 'data', 'roadnet', 'questobjs.dat'))
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()
    if args.verbose:
        sys.stdout.reconfigure(encoding='utf-8')
    modules = [p for p in (args.plugin or DEFAULT_PLUGINS)
               if os.path.exists(os.path.join(args.data, p))]
    if not modules or modules[0].lower() != 'fallout3.esm':
        raise SystemExit('Fallout3.esm must be there, first')
    d = Data(modules)
    for i, m in enumerate(modules):
        d.read(os.path.join(args.data, m), i)
        print('%-18s read' % m)

    # What each script does: quest stages set in interaction / trigger blocks,
    # and the objectives completed there.
    interact, trigger, sit = {}, {}, set()
    interact_obj, trigger_obj = {}, {}
    for sid, src in d.scripts.items():
        for name, body in BLOCK.findall(src):
            name = name.lower()
            objs = SETOBJECTIVE.findall(body)
            if objs and name in INTERACT_BLOCKS:
                interact_obj.setdefault(sid, []).extend(objs)
            elif objs and name in TRIGGER_BLOCKS:
                trigger_obj.setdefault(sid, []).extend(objs)
            sets = SETSTAGE.findall(body)
            if not sets:
                continue
            if name in INTERACT_BLOCKS:
                interact.setdefault(sid, []).extend(sets)
            elif name in TRIGGER_BLOCKS:
                trigger.setdefault(sid, []).extend(sets)
                if re.search(r'(?i)\bplayer\s*\.\s*getsitting\b', body):
                    sit.add(sid)

    entries = {}      # (ref, quest, stage, objective, kind) -> True

    def add(ref, qedid, stage, kind):
        q = d.quest_edid.get(qedid.lower())
        if q is not None:
            entries[(ref, q, int(stage) & 0xFFFF, -1, kind)] = True

    def add_obj(ref, qedid, objective, kind):
        q = d.quest_edid.get(qedid.lower())
        if q is not None:
            entries[(ref, q, 0, int(objective), kind)] = True

    by_base = {}
    for rid, r in d.refs.items():
        by_base.setdefault(r[0], []).append(rid)
    for bid, (typ, sid, edid) in d.base.items():
        if typ in (b'NPC_', b'CREA'):
            continue                                   # people are in their own categories
        kind = K_ITEM if typ in ITEM_TYPES else KIND_OF_BASE.get(typ, K_ACTIVATOR)
        sets = []
        if sid in interact:
            sets += interact[sid]
        if typ == b'TERM' and bid in d.term_sets:
            sets += d.term_sets[bid]
        for rid in by_base.get(bid, []):
            for q, st in sets:
                add(rid, q, st, kind)
            for q, obj in interact_obj.get(sid, []):
                add_obj(rid, q, obj, kind)
            if d.refs[rid][5] is not None:
                for q, obj in trigger_obj.get(sid, []):
                    add_obj(rid, q, obj, K_TRIGGER)
            if sid in trigger and d.refs[rid][5] is not None:
                for q, st in trigger[sid]:
                    add(rid, q, st, K_TRIGGER)
                if sid in sit:
                    # The free seats inside: furniture not claimed by a package.
                    for fid, f in d.refs.items():
                        fb = d.base.get(f[0])
                        if not fb or fb[0] != b'FURN' or fid in d.pack_targets:
                            continue
                        if 'marker' in fb[2] or 'lean' in fb[2]:
                            continue
                        if f[6] == d.refs[rid][6] and inside(d.refs[rid], f[1:4]):
                            for q, st in trigger[sid]:
                                add(fid, q, st, K_SEAT)
    # Objective targets that are not people.
    for q, obj, t in d.objectives:
        r = d.refs.get(t)
        if not r:
            continue                                   # an actor (ACHR) or unknown
        b = d.base.get(r[0])
        typ = b[0] if b else b'STAT'
        kind = K_ITEM if typ in ITEM_TYPES else KIND_OF_BASE.get(typ, K_MARKER)
        entries[(t, q, 0, obj, kind)] = True

    rows = sorted(entries)
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, 'wb') as f:
        f.write(MAGIC)
        f.write(struct.pack('<III', VERSION, len(modules), len(rows)))
        for m in modules:
            mb = m.encode('ascii')[:63]
            f.write(mb + b'\0' * (64 - len(mb)))
        for ref, q, st, obj, kind in rows:
            f.write(struct.pack('<IIHhB3x', ref, q, st, obj, kind))
    counts = {}
    for r in rows:
        counts[r[4]] = counts.get(r[4], 0) + 1
    names = {1: 'activators', 2: 'terminals', 3: 'doors', 4: 'trigger spots', 5: 'seats',
             6: 'markers', 7: 'items', 8: 'furniture', 9: 'containers'}
    print('%d entries: %s' % (len(rows), ', '.join('%d %s' % (n, names[k]) for k, n in sorted(counts.items()))))
    print('wrote %s (%d bytes)' % (os.path.abspath(args.output), os.path.getsize(args.output)))
    if args.verbose:
        rev = {v: k for k, v in d.quest_edid.items()}
        for ref, q, st, obj, kind in rows:
            if rev.get(q, '') in ('cg03', 'cg04'):
                r = d.refs[ref]
                print('  %s %-8s ref %08X %-24s stage %3d obj %3d at (%.0f,%.0f,%.0f)' % (
                    names[kind][:-1], rev.get(q), ref, d.base.get(r[0], ('', '', ''))[2], st, obj, *r[1:4]))


if __name__ == '__main__':
    main()

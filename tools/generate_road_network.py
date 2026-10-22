"""
Generate a road waypoint graph for a Fallout 3 worldspace (Fallout3.esm).

Adapted for Fallout3.esm (Capital Wasteland, worldspace EDID "Wasteland") from
tools/generate_road_network.py in FalloutNVAccess by Alexander Epaneshnikov
(https://github.com/FalloutAccess/FalloutNVAccess), used under the MIT License:

    MIT License

    Copyright (c) 2026 Alexander Epaneshnikov

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
    IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
    DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
    OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
    OR OTHER DEALINGS IN THE SOFTWARE.

The output is byte-compatible with FalloutNVAccess's RoadNetwork.cpp loader
(magic 'FNVR', version 2, 32-byte worldspace EDID in the header).

How roads are found
-------------------
Alex's original idea: a navmesh triangle is "road" if it lies on a landscape
texture whose LTEX editor ID looks like a road (asphalt, pavement, dirt path...).
Road triangles are binned into a 512-unit grid, one node per bin (and Z level),
adjacent bins are linked, and nearby disconnected pieces are bridged.

Fallout 3 differences (verified against Fallout3.esm 1.7 / GOTY, Polish):
  * Record and group headers are 24 bytes, LAND BTXT/ATXT/VTXT and NAVM
    NVVX/NVTR layouts are the same as in FNV (NAVM vertices are absolute world
    coordinates, NVTR is 16 bytes per triangle). The LTEX keyword list already
    matches all 8 FO3 road textures (Asphalt02/03, LAsphaltWasteland01-03,
    Pavement, LDirtPathWasteland01, GravelRoadWasteland01).
  * BUT most Capital Wasteland roads are NOT painted on the landscape: highways
    and side roads are static meshes (Landscape\\Roads\\WastelandRoad*.nif,
    ground-level Architecture\\Wasteland\\Highway\\Highway*.nif, SCOLRoad*), and
    the trails between settlements (e.g. Springvale -> Megaton) only exist as
    navmesh triangles flagged "Preferred" (0x40). Using textures alone leaves
    Megaton and many towns disconnected. This generator therefore also treats
    as road:
      - navmesh triangles with the Preferred flag (--no-preferred to disable);
      - navmesh triangles under a road mesh REFR: the base object's OBND box,
        scaled and rotated by the REFR, with a Z window (--no-road-meshes);
  * Road textures count by their VISIBLE weight (layers blend bottom-up in layer
    order) instead of raw layer opacity: in Fallout3.esm ~42% of the vertices of
    quadrants whose BASE texture is asphalt/pavement are painted over by other
    layers, which made fake road blobs (e.g. the hilltop next to Vault 101).
    --raw-opacity restores the original rule.
  * Fallout 3 "Preferred" triangles are large (median longest edge ~512 units),
    so taking one centroid per triangle left half-empty 512-unit bins and broke
    trails apart. Triangles longer than --subdivide units are split into n*n
    sub-triangles and every sub-centroid is a sample (small triangles still
    contribute exactly their centroid, as in the original).
  * Fallout3.esm has 32 worldspaces, so a worldspace is always selected
    (default: Wasteland) instead of mixing every worldspace's cells together.
  * Map markers (REFRs with XMRK) are collected for the SVG/JSON debug output.
  * bridge_components() returns the same result as the original but uses a
    spatial grid instead of scanning every component per grid cell (the
    original took many minutes on FO3 data).
  * Subrecords larger than 64 KB (XXXX prefix) are handled.
  * The ESM is read group-by-group into memory (one pass over the worldspace for
    LAND/REFR/NAVM), ~25 s for the Capital Wasteland instead of many minutes.

Regression check: with --raw-opacity --no-preferred --no-road-meshes
--subdivide 0 the output for Wasteland is byte-identical to the unmodified FNV
script run with --worldspace Wasteland.

Usage:
    python generate_road_network.py [options]

Typical (writes Wasteland.dat for the Capital Wasteland):
    python tools/generate_road_network.py --esm "<FO3>\\Data\\Fallout3.esm" ^
        --output data/roadnet/Wasteland.dat --svg out.svg --json out.json --verbose

Options:
    --esm PATH              Path to Fallout3.esm (auto-detected if not given)
    --worldspace EDID       Worldspace to process (repeatable, default Wasteland)
    --all-worldspaces       Process every worldspace that yields a road graph
    --output PATH           Output .dat (single worldspace; default <EDID>.dat)
    --output-dir DIR        Directory for <EDID>.dat files (multi-worldspace)
    --svg PATH|DIR          SVG visualisation (file for one worldspace, else dir)
    --json PATH|DIR         JSON debug dump (file for one worldspace, else dir)
    --road-edids-file PATH  File with extra road texture EDIDs (one per line)
    --verbose               Print LTEX list, road textures/meshes and statistics
    --grid-size N           Spatial bin size in game units (default: 512)
    --min-opacity F         Min visible road-texture weight to count as road (default: 0.25)
    --raw-opacity           Original FNV rule (raw layer opacity, base = whole quadrant)
    --subdivide N           Split navmesh triangles longer than N units (default 256, 0=off)
    --no-preferred          Do not use navmesh "Preferred" triangles
    --no-road-meshes        Do not use road mesh REFRs
    --road-chunks           Also treat RoadChunk*/RoadChunkCluster* debris as road
    --bridge-dist N         Max bridge length between components (default 6144)
    --no-bridge             Disable component bridging
"""

import struct
import sys
import os
import json
import math
import zlib
import argparse
from collections import defaultdict


# ─── Constants ──────────────────────────────────────────────────────────

# Triangle flags from navmesh (NVTR flags, uint16)
FLAG_PREFERRED = 0x40
FLAG_WATER = 0x200
FLAG_DOOR = 0x400

# Cell and quadrant geometry
CELL_SIZE = 4096  # game units per exterior cell
QUAD_SIZE = CELL_SIZE // 2  # 2048 units per quadrant
LAND_GRID = 17  # 17x17 height/opacity grid per quadrant (289 vertices)
LAND_VERTEX_SPACING = QUAD_SIZE / (LAND_GRID - 1)  # 128 units

# Road texture EDID keywords (case-insensitive substring matching).
# Unchanged from the FNV original: in Fallout3.esm they match exactly
# Asphalt02, Asphalt03, Pavement, LAsphaltWasteland01/02/03,
# LDirtPathWasteland01 and GravelRoadWasteland01.
ROAD_KEYWORDS = [
    'road', 'asphalt', 'pavement', 'highway', 'concrete',
    'sidewalk', 'dirtroad', 'gravelroad',
    'cobblestone', 'dirtpath',
]
# Never road even if a keyword matches (FO3 has LRubbleBrick, LRubblePile*,
# GroundLitter*; none currently match a keyword, this is a guard).
ROAD_EXCLUDE_KEYWORDS = ['rubble', 'litter']

# Navmesh triangle is "near" a road texture vertex within this 2D radius
TEXTURE_NEAR_RADIUS = 256.0
# Padding around a road mesh's OBND footprint (2D) and its Z window
ROAD_MESH_PAD = 64.0
ROAD_MESH_Z_PAD = 192.0

# Binary format magic and version (must match RoadNetwork.cpp)
MAGIC = b'FNVR'
VERSION = 2

DEFAULT_WORLDSPACE = 'Wasteland'


# ─── ESM record parsing ────────────────────────────────────────────────

FLAG_COMPRESSED = 0x00040000
HEADER_SIZE = 24  # FO3/FNV record and group headers are both 24 bytes


def read_subrecords(data):
    """Yield (type, data) for each subrecord. Handles the XXXX large-size prefix."""
    offset = 0
    n = len(data)
    big_size = None
    while offset + 6 <= n:
        sub_type = bytes(data[offset:offset + 4])
        sub_size = struct.unpack_from('<H', data, offset + 4)[0]
        offset += 6
        if sub_type == b'XXXX':
            big_size = struct.unpack_from('<I', data, offset)[0]
            offset += sub_size
            continue
        if big_size is not None:
            sub_size = big_size
            big_size = None
        yield sub_type, data[offset:offset + sub_size]
        offset += sub_size


def record_data(buf, offset, size, flags):
    """Return record data (decompressed if needed) or None on zlib failure."""
    if flags & FLAG_COMPRESSED:
        try:
            return zlib.decompress(bytes(buf[offset + 4:offset + size]))
        except zlib.error:
            return None
    return bytes(buf[offset:offset + size])


def edid_of(data):
    for sub_type, sub_data in read_subrecords(data):
        if sub_type == b'EDID':
            return sub_data.rstrip(b'\x00').decode('ascii', errors='replace')
    return ''


class EsmFile:
    """Index of an ESM's top-level groups; groups are loaded into memory on demand."""

    def __init__(self, path):
        self.path = path
        self.groups = {}  # label -> (offset, size)
        with open(path, 'rb') as f:
            file_size = f.seek(0, 2)
            f.seek(0)
            hdr = f.read(HEADER_SIZE)
            if len(hdr) < HEADER_SIZE or hdr[:4] != b'TES4':
                raise ValueError(f'{path}: not a TES4-format ESM')
            pos = HEADER_SIZE + struct.unpack_from('<I', hdr, 4)[0]
            while pos + HEADER_SIZE <= file_size:
                f.seek(pos)
                g = f.read(HEADER_SIZE)
                if g[:4] != b'GRUP':
                    raise ValueError(f'{path}: expected GRUP at {pos}, got {g[:4]!r}')
                size = struct.unpack_from('<I', g, 4)[0]
                self.groups[g[8:12]] = (pos, size)
                pos += size

    def load_group(self, label):
        """Return the raw bytes of a top-level group (header included) or None."""
        if label not in self.groups:
            return None
        pos, size = self.groups[label]
        with open(self.path, 'rb') as f:
            f.seek(pos)
            return f.read(size)


def walk_records(buf, start, end, group_stack=()):
    """Walk records/groups in buf[start:end].

    Yields ('GRUP', label_bytes, group_type, offset, size, stack) when entering a
    group and ('REC', type, flags, form_id, data_offset, size, stack) for records.
    The caller may send() True after a GRUP item to skip that group's contents.
    """
    offset = start
    while offset + HEADER_SIZE <= end:
        rec_type = bytes(buf[offset:offset + 4])
        size = struct.unpack_from('<I', buf, offset + 4)[0]
        if rec_type == b'GRUP':
            label = bytes(buf[offset + 8:offset + 12])
            group_type = struct.unpack_from('<I', buf, offset + 12)[0]
            skip = yield ('GRUP', label, group_type, offset, size, group_stack)
            if not skip:
                yield from walk_records(buf, offset + HEADER_SIZE, offset + size,
                                        group_stack + ((label, group_type),))
            offset += size
        else:
            flags, form_id = struct.unpack_from('<II', buf, offset + 8)
            yield ('REC', rec_type, flags, form_id, offset + HEADER_SIZE, size, group_stack)
            offset += HEADER_SIZE + size


def advance(walker, skip=False):
    """Next item from walk_records(); skip=True skips the group just yielded. None at end."""
    try:
        return walker.send(True) if skip else next(walker)
    except StopIteration:
        return None


# ─── Pass 1: Parse LTEX records ────────────────────────────────────────

def parse_ltex_records(esm, extra_road_edids=None):
    """Extract landscape texture {formID: editorID} mapping and identify road textures."""
    ltex_map = {}  # formID -> editorID
    road_formids = set()

    extra_edids = set()
    if extra_road_edids:
        for edid in extra_road_edids:
            extra_edids.add(edid.strip().lower())

    buf = esm.load_group(b'LTEX')
    if buf is None:
        return ltex_map, road_formids
    for item in walk_records(buf, HEADER_SIZE, len(buf)):
        if item[0] != 'REC' or item[1] != b'LTEX':
            continue
        _, _, flags, form_id, off, size, _ = item
        data = record_data(buf, off, size, flags)
        if data is None:
            continue
        edid = edid_of(data)
        if not edid:
            continue
        ltex_map[form_id] = edid
        edid_lower = edid.lower()
        is_road = any(kw in edid_lower for kw in ROAD_KEYWORDS)
        if is_road and any(kw in edid_lower for kw in ROAD_EXCLUDE_KEYWORDS):
            is_road = False
        if not is_road and edid_lower in extra_edids:
            is_road = True
        if is_road:
            road_formids.add(form_id)

    return ltex_map, road_formids


# ─── Parse WRLD records ───────────────────────────────────────────────

def parse_wrld_records(esm):
    """Extract worldspace {formID: editorID} mapping from WRLD records."""
    wrld_map = {}
    buf = esm.load_group(b'WRLD')
    if buf is None:
        return wrld_map
    # WRLD records are direct children of the top group; skip the world children groups
    walker = walk_records(buf, HEADER_SIZE, len(buf))
    item = advance(walker)
    while item is not None:
        if item[0] == 'GRUP':
            item = advance(walker, skip=True)
            continue
        if item[1] == b'WRLD':
            data = record_data(buf, item[4], item[5], item[2])
            if data is not None:
                edid = edid_of(data)
                if edid:
                    wrld_map[item[3]] = edid
        item = advance(walker)
    return wrld_map


# ─── Parse road mesh base objects (FO3) ────────────────────────────────

def is_road_mesh(edid, model, include_chunks=False):
    """Decide whether a STAT/SCOL base object is a walkable ground-level road piece.

    Fallout 3 builds most wasteland roads from static meshes. Included:
      Landscape\\Roads\\*            (WastelandRoad* straights/curves/junctions/end caps,
                                     ramps, patches, CanterburyCommonsRoad01, ...)
      SCOLRoad*                     (static collections of the above)
      Architecture\\Wasteland\\Highway\\Highway*  (ground-level highway pieces)
    Excluded: guard rails, overpasses/elevated pieces (would attach the road
    below to the deck above), raised "RW" highway pieces, beltway
    interchanges, signs, and the RoadChunk* debris (unless include_chunks).
    """
    e = edid.lower()
    m = model.lower().replace('/', '\\')
    if 'guardrail' in e or 'overpass' in e or 'overpass' in m:
        return False
    if e.startswith('roadchunk'):
        return include_chunks
    if m.startswith('landscape\\roads\\'):
        return True
    if e.startswith('scolroad'):
        return True
    if m.startswith('architecture\\wasteland\\highway\\highway'):
        if 'rw' in e[len('highway'):].lower() or 'beltway' in e or 'sign' in e:
            return False
        return True
    return False


def parse_road_mesh_bases(esm, include_chunks=False):
    """Return {formID: (edid, model, obnd)} of road mesh base objects (STAT/SCOL)."""
    bases = {}
    for label in (b'STAT', b'SCOL'):
        buf = esm.load_group(label)
        if buf is None:
            continue
        for item in walk_records(buf, HEADER_SIZE, len(buf)):
            if item[0] != 'REC' or item[1] != label:
                continue
            data = record_data(buf, item[4], item[5], item[2])
            if data is None:
                continue
            edid = ''
            model = ''
            obnd = None
            for sub_type, sub_data in read_subrecords(data):
                if sub_type == b'EDID':
                    edid = sub_data.rstrip(b'\x00').decode('ascii', errors='replace')
                elif sub_type == b'MODL':
                    model = sub_data.rstrip(b'\x00').decode('ascii', errors='replace')
                elif sub_type == b'OBND' and len(sub_data) >= 12:
                    obnd = struct.unpack_from('<6h', sub_data, 0)
            if obnd and is_road_mesh(edid, model, include_chunks):
                x1, y1, z1, x2, y2, z2 = obnd
                if x2 > x1 and y2 > y1:
                    bases[item[3]] = (edid, model, obnd)
    return bases


# ─── Pass 2: worldspace contents (LAND textures, road mesh REFRs, markers) ──

def parse_worldspaces(esm, worldspace_filter, road_formids, road_mesh_bases,
                      min_opacity=0.25, name_encoding='cp1250', raw_opacity=False):
    """Walk the WRLD group once for the selected worldspaces.

    Returns {ws_formid: {
        'road_positions': set((x, y)),           # road texture vertices (Alex's pass 2)
        'road_cells': set((cellX, cellY)),
        'meshes': [(x, y, z, rotZ, scale, obnd, base_formid)],
        'markers': [(name, x, y, z, form_id)],
        'navm': [(data_offset, size, flags, cellX, cellY)],
        'texture_layers': int,
    }} and the WRLD buffer (needed again for the NAVM pass).
    """
    buf = esm.load_group(b'WRLD')
    result = {}
    if buf is None:
        return result, None

    quad_offsets = {
        0: (0, 0),                  # SW (bottom-left)
        1: (QUAD_SIZE, 0),          # SE (bottom-right)
        2: (0, QUAD_SIZE),          # NW (top-left)
        3: (QUAD_SIZE, QUAD_SIZE),  # NE (top-right)
    }

    current_ws = None
    current_cell = None
    ws_data = None

    walker = walk_records(buf, HEADER_SIZE, len(buf))
    item = advance(walker)
    while item is not None:
        if item[0] == 'GRUP':
            _, label, group_type, _, _, stack = item
            if group_type == 1:  # world children (label = WRLD FormID)
                ws_formid = struct.unpack('<I', label)[0]
                if ws_formid not in worldspace_filter:
                    item = advance(walker, skip=True)
                    continue
                current_ws = ws_formid
                current_cell = None
                ws_data = result.setdefault(ws_formid, {
                    'road_positions': set(), 'road_cells': set(), 'meshes': [],
                    'markers': [], 'navm': [], 'texture_layers': 0})
            item = advance(walker)
            continue

        _, rec_type, flags, form_id, off, size, stack = item
        item = advance(walker)
        if current_ws is None or not stack or stack[0][1] != 1:
            continue

        if rec_type == b'CELL':
            data = record_data(buf, off, size, flags)
            current_cell = None
            if data is None:
                continue
            for sub_type, sub_data in read_subrecords(data):
                if sub_type == b'XCLC' and len(sub_data) >= 8:
                    current_cell = struct.unpack_from('<ii', sub_data, 0)
                    break

        elif rec_type == b'LAND':
            if current_cell is None or not road_formids:
                continue
            data = record_data(buf, off, size, flags)
            if data is None:
                continue
            # ATXT = additional texture, BTXT = base texture, VTXT = per-vertex
            # opacity for the preceding ATXT (same layout in FO3 and FNV):
            #   BTXT/ATXT: ltex FormID(4), quadrant(1), unknown(1), layer(int16)
            #   VTXT: entries of position(uint16) + pad(2) + opacity(float);
            #         position = row*17 + col, row 0 = south edge of the quadrant
            # quadrant -> [(ltex, is_base, layer, vtxt_bytes)]
            quad_textures = defaultdict(list)
            atxt = None
            for sub_type, sub_data in read_subrecords(data):
                if sub_type == b'BTXT' and len(sub_data) >= 8:
                    ltex = struct.unpack_from('<I', sub_data, 0)[0]
                    quad_textures[sub_data[4]].append((ltex, True, -1, None))
                elif sub_type == b'ATXT' and len(sub_data) >= 8:
                    ltex, quadrant, _unk, layer = struct.unpack_from('<IBBh', sub_data, 0)
                    atxt = (ltex, quadrant, layer)
                elif sub_type == b'VTXT' and atxt is not None:
                    quad_textures[atxt[1]].append((atxt[0], False, atxt[2], sub_data))
                    atxt = None

            cell_x, cell_y = current_cell
            cell_world_x = cell_x * CELL_SIZE
            cell_world_y = cell_y * CELL_SIZE
            pos_set = ws_data['road_positions']
            for quadrant, layers in quad_textures.items():
                if quadrant not in quad_offsets:
                    continue
                if not any(ltex in road_formids for ltex, _, _, _ in layers):
                    continue
                qx_off, qy_off = quad_offsets[quadrant]
                ws_data['road_cells'].add(current_cell)

                def decode(vtxt):
                    return {pos: op for pos, _, op in struct.iter_unpack(
                        '<HHf', vtxt[:len(vtxt) // 8 * 8])}

                road_weight = [0.0] * (LAND_GRID * LAND_GRID)
                if raw_opacity:
                    # FNV original: a road base texture marks the whole quadrant and a
                    # road ATXT counts wherever its own opacity >= min_opacity,
                    # regardless of what is painted on top of it.
                    for ltex, is_base, layer, vtxt in layers:
                        if ltex not in road_formids:
                            continue
                        ws_data['texture_layers'] += 1
                        if is_base:
                            road_weight = [1.0] * len(road_weight)
                        else:
                            for pos, op in decode(vtxt).items():
                                if 0 <= pos < len(road_weight):
                                    road_weight[pos] = max(road_weight[pos], op)
                else:
                    # Visible weight: layers blend bottom-up in layer order, so layer i
                    # shows with op_i * prod(1 - op_j) over the layers above it and the
                    # base texture with prod(1 - op_j) over all layers. In Fallout3.esm
                    # ~42% of road-base vertices are painted over (visible < 0.25) -
                    # e.g. an asphalt-based hilltop next to Vault 101.
                    base = [l for l in layers if l[1]]
                    additional = sorted((l for l in layers if not l[1]), key=lambda l: l[2])
                    remaining = [1.0] * len(road_weight)
                    for ltex, is_base, layer, vtxt in reversed(additional):
                        is_road = ltex in road_formids
                        if is_road:
                            ws_data['texture_layers'] += 1
                        for pos, op in decode(vtxt).items():
                            if 0 <= pos < len(road_weight):
                                if is_road:
                                    road_weight[pos] += op * remaining[pos]
                                remaining[pos] *= (1.0 - op)
                    if base and base[0][0] in road_formids:
                        ws_data['texture_layers'] += 1
                        for pos in range(len(road_weight)):
                            road_weight[pos] += remaining[pos]

                for pos, weight in enumerate(road_weight):
                    if weight >= min_opacity:
                        row = pos // LAND_GRID
                        col = pos % LAND_GRID
                        pos_set.add((cell_world_x + qx_off + col * LAND_VERTEX_SPACING,
                                     cell_world_y + qy_off + row * LAND_VERTEX_SPACING))

        elif rec_type == b'REFR':
            data = record_data(buf, off, size, flags)
            if data is None:
                continue
            base = None
            pos = None
            scale = 1.0
            is_marker = False
            name = ''
            for sub_type, sub_data in read_subrecords(data):
                if sub_type == b'NAME' and len(sub_data) >= 4:
                    base = struct.unpack_from('<I', sub_data, 0)[0]
                elif sub_type == b'DATA' and len(sub_data) >= 24:
                    pos = struct.unpack_from('<6f', sub_data, 0)
                elif sub_type == b'XSCL' and len(sub_data) >= 4:
                    scale = struct.unpack_from('<f', sub_data, 0)[0]
                elif sub_type == b'XMRK':
                    is_marker = True
                elif sub_type == b'FULL':
                    name = sub_data.rstrip(b'\x00').decode(name_encoding, errors='replace')
            if pos is None:
                continue
            if flags & 0x20:  # deleted reference
                continue
            if is_marker:
                ws_data['markers'].append((name, pos[0], pos[1], pos[2], form_id))
            if base in road_mesh_bases:
                ws_data['meshes'].append((pos[0], pos[1], pos[2], pos[5], scale,
                                          road_mesh_bases[base][2], base))

        elif rec_type == b'NAVM':
            if flags & 0x20:  # deleted navmesh
                continue
            cx, cy = current_cell if current_cell else (None, None)
            ws_data['navm'].append((off, size, flags, cx, cy))

    return result, buf


# ─── Pass 3: navmesh triangles on road ─────────────────────────────────

ROAD_GRID_SIZE = 256  # spatial hash cell for road texture vertices / mesh boxes

_SUBDIV_CACHE = {}


def subdivision_barycentrics(n):
    """Barycentric (a, b, c) of the centroids of the n*n sub-triangles of a triangle."""
    if n in _SUBDIV_CACHE:
        return _SUBDIV_CACHE[n]
    out = []
    if n == 1:
        _SUBDIV_CACHE[n] = None  # caller uses the plain centroid (a + b + c) / 3
        return None
    for i in range(n):
        for j in range(n - i):
            a = (i + 1.0 / 3.0) / n
            b = (j + 1.0 / 3.0) / n
            out.append((a, b, 1.0 - a - b))
            if i + j < n - 1:
                a = (i + 2.0 / 3.0) / n
                b = (j + 2.0 / 3.0) / n
                out.append((a, b, 1.0 - a - b))
    _SUBDIV_CACHE[n] = out
    return out


class RoadIndex:
    """Spatial lookup of road signals for one worldspace."""

    def __init__(self, road_positions, meshes, texture_radius=TEXTURE_NEAR_RADIUS):
        self.texture_radius = texture_radius
        self.tex_grid = defaultdict(list)
        for wx, wy in road_positions:
            self.tex_grid[(int(math.floor(wx / ROAD_GRID_SIZE)),
                           int(math.floor(wy / ROAD_GRID_SIZE)))].append((wx, wy))
        # cells whose 3x3 neighbourhood holds a texture vertex (exact for radius <= cell)
        self.r_cells = int(math.ceil(texture_radius / ROAD_GRID_SIZE))
        self.tex_candidates = set()
        for (gx, gy) in self.tex_grid:
            for dx in range(-self.r_cells, self.r_cells + 1):
                for dy in range(-self.r_cells, self.r_cells + 1):
                    self.tex_candidates.add((gx + dx, gy + dy))

        # road mesh footprints: oriented boxes registered in every cell they touch
        self.mesh_grid = defaultdict(list)
        self.mesh_count = 0
        for (x, y, z, rot_z, scale, obnd, base) in meshes:
            x1, y1, z1, x2, y2, z2 = [v * scale for v in obnd]
            c, s = math.cos(rot_z), math.sin(rot_z)
            box = (x, y, c, s,
                   x1 - ROAD_MESH_PAD, x2 + ROAD_MESH_PAD,
                   y1 - ROAD_MESH_PAD, y2 + ROAD_MESH_PAD,
                   z + z1 - ROAD_MESH_Z_PAD, z + z2 + ROAD_MESH_Z_PAD)
            # Bethesda Z rotation is clockwise seen from above:
            #   world = (x + lx*cos + ly*sin, y - lx*sin + ly*cos)
            corners = [(x + lx * c + ly * s, y - lx * s + ly * c)
                       for lx in (box[4], box[5]) for ly in (box[6], box[7])]
            gx0 = int(math.floor(min(p[0] for p in corners) / ROAD_GRID_SIZE))
            gx1 = int(math.floor(max(p[0] for p in corners) / ROAD_GRID_SIZE))
            gy0 = int(math.floor(min(p[1] for p in corners) / ROAD_GRID_SIZE))
            gy1 = int(math.floor(max(p[1] for p in corners) / ROAD_GRID_SIZE))
            for gx in range(gx0, gx1 + 1):
                for gy in range(gy0, gy1 + 1):
                    self.mesh_grid[(gx, gy)].append(box)
            self.mesh_count += 1

    def triangle_may_touch(self, xs, ys):
        """Cheap reject: does the triangle's bounding box touch any candidate cell?"""
        gx0 = int(math.floor(min(xs) / ROAD_GRID_SIZE))
        gx1 = int(math.floor(max(xs) / ROAD_GRID_SIZE))
        gy0 = int(math.floor(min(ys) / ROAD_GRID_SIZE))
        gy1 = int(math.floor(max(ys) / ROAD_GRID_SIZE))
        for gx in range(gx0, gx1 + 1):
            for gy in range(gy0, gy1 + 1):
                if (gx, gy) in self.tex_candidates or (gx, gy) in self.mesh_grid:
                    return True
        return False

    def near_texture(self, x, y):
        gx = int(math.floor(x / ROAD_GRID_SIZE))
        gy = int(math.floor(y / ROAD_GRID_SIZE))
        if (gx, gy) not in self.tex_candidates:
            return False
        r2 = self.texture_radius * self.texture_radius
        for dx in range(-self.r_cells, self.r_cells + 1):
            for dy in range(-self.r_cells, self.r_cells + 1):
                for rx, ry in self.tex_grid.get((gx + dx, gy + dy), ()):
                    if (x - rx) ** 2 + (y - ry) ** 2 <= r2:
                        return True
        return False

    def on_mesh(self, x, y, z):
        boxes = self.mesh_grid.get((int(math.floor(x / ROAD_GRID_SIZE)),
                                    int(math.floor(y / ROAD_GRID_SIZE))))
        if not boxes:
            return False
        for (bx, by, c, s, lx0, lx1, ly0, ly1, z0, z1) in boxes:
            if not (z0 <= z <= z1):
                continue
            dx = x - bx
            dy = y - by
            lx = dx * c - dy * s
            ly = dx * s + dy * c
            if lx0 <= lx <= lx1 and ly0 <= ly <= ly1:
                return True
        return False


def parse_navm_road_triangles(buf, navm_records, index, use_preferred=True,
                              subdivide=256.0, stats=None):
    """Return road samples [(x, y, z)] from the worldspace's NAVM records.

    A triangle (water and door triangles skipped, as in the original) is road if
    it is flagged Preferred, or its sample lies within TEXTURE_NEAR_RADIUS of a
    road texture vertex, or inside a road mesh footprint. Triangles whose longest
    2D edge exceeds `subdivide` are split into n*n sub-triangles (n = ceil(edge /
    subdivide)) and each sub-centroid is tested and emitted separately.
    """
    if stats is None:
        stats = {}
    samples = []
    counts = defaultdict(int)
    for (off, size, flags, cell_x, cell_y) in navm_records:
        data = record_data(buf, off, size, flags)
        counts['navm'] += 1
        if data is None:
            continue
        nvvx = None
        nvtr = None
        for sub_type, sub_data in read_subrecords(data):
            if sub_type == b'NVVX':
                nvvx = sub_data
            elif sub_type == b'NVTR':
                nvtr = sub_data
        if not nvvx or not nvtr:
            continue
        verts = list(struct.iter_unpack('<fff', nvvx[:len(nvvx) // 12 * 12]))
        nverts = len(verts)
        # NVTR: 3 x int16 vertex, 3 x int16 edge link, uint16 flags, uint16 cover flags
        for v0, v1, v2, _e0, _e1, _e2, tri_flags, _cover in struct.iter_unpack(
                '<hhhhhhHH', nvtr[:len(nvtr) // 16 * 16]):
            counts['triangles'] += 1
            if tri_flags & (FLAG_WATER | FLAG_DOOR):
                continue
            if not (0 <= v0 < nverts and 0 <= v1 < nverts and 0 <= v2 < nverts):
                counts['bad_index'] += 1
                continue
            a = verts[v0]
            b = verts[v1]
            c = verts[v2]
            preferred = use_preferred and (tri_flags & FLAG_PREFERRED) != 0
            xs = (a[0], b[0], c[0])
            ys = (a[1], b[1], c[1])
            if not preferred and not index.triangle_may_touch(xs, ys):
                continue
            n = 1
            if subdivide and subdivide > 0:
                longest = max(math.hypot(a[0] - b[0], a[1] - b[1]),
                              math.hypot(b[0] - c[0], b[1] - c[1]),
                              math.hypot(c[0] - a[0], c[1] - a[1]))
                n = max(1, int(math.ceil(longest / subdivide)))
            tri_hit = False
            weights = subdivision_barycentrics(n)
            if weights is None:
                # small triangle: exactly the original's centroid
                points = (((a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3,
                           (a[2] + b[2] + c[2]) / 3),)
            else:
                points = [(a[0] * wa + b[0] * wb + c[0] * wc,
                           a[1] * wa + b[1] * wb + c[1] * wc,
                           a[2] * wa + b[2] * wb + c[2] * wc) for wa, wb, wc in weights]
            for x, y, z in points:
                if preferred:
                    src = 'preferred'
                elif index.near_texture(x, y):
                    src = 'texture'
                elif index.on_mesh(x, y, z):
                    src = 'mesh'
                else:
                    continue
                samples.append((x, y, z))
                counts['samples_' + src] += 1
                tri_hit = True
            if tri_hit:
                counts['road_triangles'] += 1
    stats.update(counts)
    return samples


# ─── Graph construction (unchanged from the FNV original) ──────────────

def build_road_graph(road_centroids, grid_size=512, z_tolerance=256, min_tris_per_cell=2):
    """Build waypoint graph from road triangle centroids.

    1. Spatial binning into grid cells
    2. Create node at average centroid per cell (with Z-level separation)
    3. Connect adjacent cells (8-connectivity)
    4. Simplify degree-2 nodes on straight segments
    5. Remove tiny isolated components
    """
    # Step 1: Bin centroids by grid cell with Z-level separation
    cell_centroids = defaultdict(list)  # (gx, gy, z_level) -> list of (x, y, z)
    for cx, cy, cz in road_centroids:
        gx = int(math.floor(cx / grid_size))
        gy = int(math.floor(cy / grid_size))
        cell_centroids[(gx, gy)].append((cx, cy, cz))

    # Z-level separation within each 2D cell
    nodes = []  # list of (x, y, z)
    node_grid = {}  # (gx, gy, z_level) -> node index

    for (gx, gy), centroids in cell_centroids.items():
        if len(centroids) < min_tris_per_cell:
            continue

        # Sort by Z and group into levels
        centroids.sort(key=lambda c: c[2])
        z_groups = []
        current_group = [centroids[0]]

        for i in range(1, len(centroids)):
            if centroids[i][2] - current_group[0][2] > z_tolerance:
                z_groups.append(current_group)
                current_group = [centroids[i]]
            else:
                current_group.append(centroids[i])
        z_groups.append(current_group)

        for z_level, group in enumerate(z_groups):
            if len(group) < min_tris_per_cell:
                continue
            avg_x = sum(c[0] for c in group) / len(group)
            avg_y = sum(c[1] for c in group) / len(group)
            avg_z = sum(c[2] for c in group) / len(group)
            node_idx = len(nodes)
            nodes.append((avg_x, avg_y, avg_z))
            node_grid[(gx, gy, z_level)] = node_idx

    print(f"Created {len(nodes)} nodes from spatial binning")

    # Step 2: Connect adjacent cells (8-connectivity)
    edges = set()  # set of (a, b) with a < b
    for (gx, gy, z_level), idx_a in node_grid.items():
        for dx in range(-1, 2):
            for dy in range(-1, 2):
                if dx == 0 and dy == 0:
                    continue
                # Try same Z-level first, then nearby Z-levels
                for dz in range(3):  # 0, 1, 2 Z-levels away
                    key = (gx + dx, gy + dy, z_level + dz)
                    if key in node_grid:
                        idx_b = node_grid[key]
                        edge = (min(idx_a, idx_b), max(idx_a, idx_b))
                        edges.add(edge)
                        break
                    if dz > 0:
                        key = (gx + dx, gy + dy, z_level - dz)
                        if key in node_grid:
                            idx_b = node_grid[key]
                            edge = (min(idx_a, idx_b), max(idx_a, idx_b))
                            edges.add(edge)
                            break

    # Compute edge weights
    weighted_edges = []
    for a, b in edges:
        dx = nodes[a][0] - nodes[b][0]
        dy = nodes[a][1] - nodes[b][1]
        dz = nodes[a][2] - nodes[b][2]
        weight = math.sqrt(dx * dx + dy * dy + dz * dz)
        weighted_edges.append((a, b, weight))

    print(f"Created {len(weighted_edges)} edges")

    # Step 3: Simplify degree-2 nodes on straight segments
    nodes, weighted_edges = simplify_graph(nodes, weighted_edges, angle_threshold=15.0)

    # Step 4: Remove small isolated components
    nodes, weighted_edges = remove_small_components(nodes, weighted_edges, min_component_size=3)

    # Step 5: Count connected components for diagnostics
    if nodes:
        print(f"Connected components: {count_components(nodes, weighted_edges)}")

    return nodes, weighted_edges


def count_components(nodes, edges):
    adj = defaultdict(set)
    for a, b, w in edges:
        adj[a].add(b)
        adj[b].add(a)
    visited = set()
    comp_count = 0
    for start in range(len(nodes)):
        if start in visited:
            continue
        comp_count += 1
        queue = [start]
        while queue:
            n = queue.pop()
            if n in visited:
                continue
            visited.add(n)
            for nb in adj[n]:
                if nb not in visited:
                    queue.append(nb)
    return comp_count


def simplify_graph(nodes, edges, angle_threshold=15.0):
    """Remove degree-2 nodes where the road is nearly straight."""
    # Build adjacency
    adj = defaultdict(set)
    edge_weights = {}
    for a, b, w in edges:
        adj[a].add(b)
        adj[b].add(a)
        edge_weights[(min(a, b), max(a, b))] = w

    # Find degree-2 nodes to remove
    to_remove = set()
    cos_threshold = math.cos(math.radians(180.0 - angle_threshold))

    for node_idx in range(len(nodes)):
        neighbors = list(adj[node_idx])
        if len(neighbors) != 2:
            continue

        n1, n2 = neighbors
        # Vector from node to each neighbor
        dx1 = nodes[n1][0] - nodes[node_idx][0]
        dy1 = nodes[n1][1] - nodes[node_idx][1]
        dx2 = nodes[n2][0] - nodes[node_idx][0]
        dy2 = nodes[n2][1] - nodes[node_idx][1]

        len1 = math.sqrt(dx1 * dx1 + dy1 * dy1)
        len2 = math.sqrt(dx2 * dx2 + dy2 * dy2)
        if len1 < 0.001 or len2 < 0.001:
            to_remove.add(node_idx)
            continue

        # Angle between the two edges (we want nearly 180 degrees = straight)
        cos_angle = (dx1 * dx2 + dy1 * dy2) / (len1 * len2)
        if cos_angle <= cos_threshold:
            to_remove.add(node_idx)

    if not to_remove:
        return nodes, edges

    # Rebuild graph without removed nodes
    # For each removed degree-2 node, connect its two neighbors
    new_edges_set = set()
    for a, b, w in edges:
        if a not in to_remove and b not in to_remove:
            new_edges_set.add((min(a, b), max(a, b), w))

    for node_idx in to_remove:
        neighbors = list(adj[node_idx])
        if len(neighbors) == 2:
            n1, n2 = neighbors
            if n1 not in to_remove and n2 not in to_remove:
                dx = nodes[n1][0] - nodes[n2][0]
                dy = nodes[n1][1] - nodes[n2][1]
                dz = nodes[n1][2] - nodes[n2][2]
                w = math.sqrt(dx * dx + dy * dy + dz * dz)
                new_edges_set.add((min(n1, n2), max(n1, n2), w))

    # Re-index nodes (compact, removing gaps)
    old_to_new = {}
    new_nodes = []
    for i in range(len(nodes)):
        if i not in to_remove:
            old_to_new[i] = len(new_nodes)
            new_nodes.append(nodes[i])

    new_edges = []
    for a, b, w in new_edges_set:
        if a in old_to_new and b in old_to_new:
            na = old_to_new[a]
            nb = old_to_new[b]
            new_edges.append((min(na, nb), max(na, nb), w))

    print(f"Simplified: removed {len(to_remove)} degree-2 nodes, "
          f"{len(new_nodes)} nodes and {len(new_edges)} edges remaining")
    return new_nodes, new_edges


def remove_small_components(nodes, edges, min_component_size=3):
    """Remove connected components with fewer than min_component_size nodes."""
    if not nodes:
        return nodes, edges

    adj = defaultdict(set)
    for a, b, w in edges:
        adj[a].add(b)
        adj[b].add(a)

    # Find connected components via BFS
    visited = set()
    components = []
    for start in range(len(nodes)):
        if start in visited:
            continue
        component = set()
        queue = [start]
        while queue:
            node = queue.pop()
            if node in visited:
                continue
            visited.add(node)
            component.add(node)
            for neighbor in adj[node]:
                if neighbor not in visited:
                    queue.append(neighbor)
        components.append(component)

    # Keep only large components
    keep = set()
    removed_components = 0
    for comp in components:
        if len(comp) >= min_component_size:
            keep.update(comp)
        else:
            removed_components += 1

    if removed_components > 0:
        print(f"Removed {removed_components} small components (< {min_component_size} nodes)")

    # Re-index
    old_to_new = {}
    new_nodes = []
    for i in range(len(nodes)):
        if i in keep:
            old_to_new[i] = len(new_nodes)
            new_nodes.append(nodes[i])

    new_edges = []
    for a, b, w in edges:
        if a in old_to_new and b in old_to_new:
            na = old_to_new[a]
            nb = old_to_new[b]
            new_edges.append((min(na, nb), max(na, nb), w))

    return new_nodes, new_edges


def bridge_components(nodes, edges, max_bridge_dist=6144.0, max_bridges_per_comp=3):
    """Bridge disconnected graph components by adding edges between nearby nodes.

    Same algorithm as the FNV original: for each component, find the closest
    node in any other component (2D); if within max_bridge_dist, add a bridge
    edge with a 1.5x weight penalty (to prefer real road edges in routing).
    Iterates until no new bridges can be added, since bridging two components
    may bring a third close enough to bridge.

    Rewritten search: one spatial grid of all nodes, grid cells visited in order
    of their minimum possible distance with early exit. The original looped over
    every component inside every grid cell, which took many minutes on FO3 data.

    Returns (nodes, edges, bridges) where bridges is the list of added edges.
    """
    all_bridges = []
    if not nodes or not edges:
        return nodes, edges, all_bridges

    GRID_SIZE = 1024.0
    search_radius = int(max_bridge_dist / GRID_SIZE) + 1
    # neighbour cell offsets sorted by the smallest distance two points in them can have
    offsets = []
    for dx in range(-search_radius, search_radius + 1):
        for dy in range(-search_radius, search_radius + 1):
            ex = max(0, abs(dx) - 1) * GRID_SIZE
            ey = max(0, abs(dy) - 1) * GRID_SIZE
            offsets.append((math.hypot(ex, ey), dx, dy))
    offsets.sort()

    while True:
        # Build adjacency and find connected components
        adj = defaultdict(set)
        for a, b, w in edges:
            adj[a].add(b)
            adj[b].add(a)

        node_to_comp = [-1] * len(nodes)
        components = []
        for start in range(len(nodes)):
            if node_to_comp[start] >= 0:
                continue
            comp_idx = len(components)
            component = [start]
            node_to_comp[start] = comp_idx
            queue = [start]
            while queue:
                n = queue.pop()
                for nb in adj[n]:
                    if node_to_comp[nb] < 0:
                        node_to_comp[nb] = comp_idx
                        component.append(nb)
                        queue.append(nb)
            components.append(component)

        if len(components) <= 1:
            break  # Fully connected

        grid = defaultdict(list)  # (gx, gy) -> [node index]
        for i, (nx, ny, nz) in enumerate(nodes):
            grid[(int(math.floor(nx / GRID_SIZE)), int(math.floor(ny / GRID_SIZE)))].append(i)

        # For each component, closest node pair to any other component
        comp_bridges = {}  # comp_idx -> (distance, node_a, node_b, other_comp)
        for comp_idx, component in enumerate(components):
            best_dist = max_bridge_dist
            best = None
            for node_idx in component:
                nx, ny, nz = nodes[node_idx]
                gx = int(math.floor(nx / GRID_SIZE))
                gy = int(math.floor(ny / GRID_SIZE))
                for lower_bound, dx, dy in offsets:
                    if lower_bound >= best_dist:
                        break
                    for other_idx in grid.get((gx + dx, gy + dy), ()):
                        other_comp = node_to_comp[other_idx]
                        if other_comp == comp_idx:
                            continue
                        ox, oy, oz = nodes[other_idx]
                        dist = math.sqrt((nx - ox) ** 2 + (ny - oy) ** 2)
                        if dist < best_dist:
                            best_dist = dist
                            best = (node_idx, other_idx, other_comp)
            if best:
                comp_bridges[comp_idx] = (best_dist, best[0], best[1], best[2])

        # Add the bridges (avoiding duplicates — if comp A wants to bridge to B
        # and B wants to bridge to A, only add one)
        bridges_this_round = []
        bridges_per_comp = defaultdict(int)
        bridged_pairs = set()
        for comp_idx in sorted(comp_bridges.keys(), key=lambda c: comp_bridges[c][0]):
            dist, node_a, node_b, other_comp = comp_bridges[comp_idx]
            pair = (min(comp_idx, other_comp), max(comp_idx, other_comp))
            if pair in bridged_pairs:
                continue
            if bridges_per_comp[comp_idx] >= max_bridges_per_comp:
                continue
            if bridges_per_comp[other_comp] >= max_bridges_per_comp:
                continue

            # Add bridge edge with 1.5x weight penalty
            weight = dist * 1.5
            edge = (min(node_a, node_b), max(node_a, node_b), weight)
            edges.append(edge)
            bridges_this_round.append(edge)
            bridged_pairs.add(pair)
            bridges_per_comp[comp_idx] += 1
            bridges_per_comp[other_comp] += 1

        if not bridges_this_round:
            break  # No more bridges possible

        all_bridges.extend(bridges_this_round)
        print(f"  Added {len(bridges_this_round)} bridges (components: {len(components)} -> ...)")

    final_comp_count = count_components(nodes, edges)
    print(f"Bridging complete: {len(all_bridges)} bridges added, {final_comp_count} components remaining")
    return nodes, edges, all_bridges


# ─── Binary output (format unchanged: RoadNetwork.cpp v2) ──────────────

def build_grid_index(nodes, edges, grid_cell_size):
    """Build spatial grid index for fast nearest-node lookup at runtime."""
    if not nodes:
        return 0, 0, 0.0, 0.0, [], [], []

    min_x = min(n[0] for n in nodes)
    min_y = min(n[1] for n in nodes)
    max_x = max(n[0] for n in nodes)
    max_y = max(n[1] for n in nodes)

    grid_dim_x = int(math.ceil((max_x - min_x) / grid_cell_size)) + 1
    grid_dim_y = int(math.ceil((max_y - min_y) / grid_cell_size)) + 1

    # Bin nodes into grid cells
    grid_cells = defaultdict(list)
    for i, (nx, ny, nz) in enumerate(nodes):
        gx = int((nx - min_x) / grid_cell_size)
        gy = int((ny - min_y) / grid_cell_size)
        gx = min(gx, grid_dim_x - 1)
        gy = min(gy, grid_dim_y - 1)
        grid_cells[(gx, gy)].append(i)

    # Flatten into offset/count arrays
    cell_offsets = []
    cell_counts = []
    node_list = []
    for gy in range(grid_dim_y):
        for gx in range(grid_dim_x):
            cell_offsets.append(len(node_list))
            cell_nodes = grid_cells.get((gx, gy), [])
            cell_counts.append(len(cell_nodes))
            node_list.extend(cell_nodes)

    return grid_dim_x, grid_dim_y, min_x, min_y, cell_offsets, cell_counts, node_list


def write_binary(output_path, nodes, edges, grid_cell_size, worldspace_edid=DEFAULT_WORLDSPACE):
    """Write road network to binary file (v2 format with worldspace EDID)."""
    grid_dim_x, grid_dim_y, min_x, min_y, cell_offsets, cell_counts, node_list = \
        build_grid_index(nodes, edges, grid_cell_size)

    max_x = min_x + grid_dim_x * grid_cell_size if nodes else 0
    max_y = min_y + grid_dim_y * grid_cell_size if nodes else 0

    # Encode EDID as 32-byte null-padded ASCII
    edid_bytes = worldspace_edid.encode('ascii', errors='replace')[:32]
    edid_bytes = edid_bytes.ljust(32, b'\x00')

    out_dir = os.path.dirname(os.path.abspath(output_path))
    os.makedirs(out_dir, exist_ok=True)
    with open(output_path, 'wb') as f:
        # Header (72 bytes)
        f.write(MAGIC)                                    # 4: magic
        f.write(struct.pack('<I', VERSION))                # 4: version
        f.write(struct.pack('<I', len(nodes)))             # 4: nodeCount
        f.write(struct.pack('<I', len(edges)))             # 4: edgeCount
        f.write(struct.pack('<f', grid_cell_size))         # 4: gridCellSize
        f.write(struct.pack('<f', min_x))                  # 4: minX
        f.write(struct.pack('<f', min_y))                  # 4: minY
        f.write(struct.pack('<f', max_x))                  # 4: maxX
        f.write(struct.pack('<f', max_y))                  # 4: maxY
        f.write(struct.pack('<I', 0))                      # 4: reserved
        f.write(edid_bytes)                                # 32: worldspaceEDID

        # Nodes (nodeCount * 12 bytes)
        for x, y, z in nodes:
            f.write(struct.pack('<fff', x, y, z))

        # Edges (edgeCount * 12 bytes)
        for a, b, w in edges:
            f.write(struct.pack('<IIf', a, b, w))

        # Grid index
        f.write(struct.pack('<II', grid_dim_x, grid_dim_y))
        for offset in cell_offsets:
            f.write(struct.pack('<I', offset))
        for count in cell_counts:
            f.write(struct.pack('<I', count))
        for node_idx in node_list:
            f.write(struct.pack('<I', node_idx))

    file_size = os.path.getsize(output_path)
    print(f"Wrote {output_path} ({file_size:,} bytes)")
    print(f"  {len(nodes)} nodes, {len(edges)} edges")
    print(f"  Grid: {grid_dim_x}x{grid_dim_y} cells of {grid_cell_size} units")


# ─── SVG visualization ──────────────────────────────────────────────────

def _xml_escape(s):
    return (s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
            .replace('"', '&quot;'))


def write_svg(svg_path, nodes, edges, road_centroids=None, bridges=None, markers=None,
              title=''):
    """Write SVG visualization of road network.

    Grey dots: road samples (thinned to one per 256 units), blue lines: road
    edges, orange dashed: bridge edges, red dots: nodes, green triangles: map markers.
    """
    if not nodes:
        print("No nodes to visualize")
        return

    min_x = min(n[0] for n in nodes) - 1000
    min_y = min(n[1] for n in nodes) - 1000
    max_x = max(n[0] for n in nodes) + 1000
    max_y = max(n[1] for n in nodes) + 1000

    width = max_x - min_x
    height = max_y - min_y

    # Scale to reasonable SVG size
    svg_width = 3000
    svg_height = int(svg_width * height / width) if width > 0 else 3000
    scale = svg_width / width

    def tx(x):
        return (x - min_x) * scale

    def ty(y):
        # Flip Y axis (SVG Y increases downward, game Y increases upward)
        return (max_y - y) * scale

    bridge_set = set((a, b) for a, b, w in (bridges or []))

    with open(svg_path, 'w', encoding='utf-8') as f:
        f.write(f'<svg xmlns="http://www.w3.org/2000/svg" '
                f'width="{svg_width}" height="{svg_height}" '
                f'viewBox="0 0 {svg_width} {svg_height}">\n')
        f.write(f'<title>{_xml_escape(title)}</title>\n')
        f.write('<rect width="100%" height="100%" fill="#1a1a1a"/>\n')

        # Exterior cell grid (every 4 cells) for orientation
        f.write('<g stroke="#2a2a2a" stroke-width="1">\n')
        gx0 = int(math.floor(min_x / CELL_SIZE / 4)) * 4
        gx1 = int(math.ceil(max_x / CELL_SIZE))
        for gx in range(gx0, gx1 + 1, 4):
            f.write(f'  <line x1="{tx(gx * CELL_SIZE):.1f}" y1="0" x2="{tx(gx * CELL_SIZE):.1f}" '
                    f'y2="{svg_height}"/>\n')
        gy0 = int(math.floor(min_y / CELL_SIZE / 4)) * 4
        gy1 = int(math.ceil(max_y / CELL_SIZE))
        for gy in range(gy0, gy1 + 1, 4):
            f.write(f'  <line x1="0" y1="{ty(gy * CELL_SIZE):.1f}" x2="{svg_width}" '
                    f'y2="{ty(gy * CELL_SIZE):.1f}"/>\n')
        f.write('</g>\n')

        # Raw road samples, thinned to one dot per 256-unit cell
        if road_centroids:
            seen = set()
            f.write('<g fill="#555">\n')
            for cx, cy, cz in road_centroids:
                key = (int(cx // 256), int(cy // 256))
                if key in seen:
                    continue
                seen.add(key)
                f.write(f'  <circle cx="{tx(cx):.1f}" cy="{ty(cy):.1f}" r="0.8"/>\n')
            f.write('</g>\n')

        # Draw edges
        f.write('<g stroke="#4488ff" stroke-width="1.5" opacity="0.8">\n')
        for a, b, w in edges:
            if (a, b) in bridge_set:
                continue
            x1, y1 = tx(nodes[a][0]), ty(nodes[a][1])
            x2, y2 = tx(nodes[b][0]), ty(nodes[b][1])
            f.write(f'  <line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}"/>\n')
        f.write('</g>\n')
        if bridge_set:
            f.write('<g stroke="#ff9900" stroke-width="2" stroke-dasharray="6,4" opacity="0.9">\n')
            for a, b in bridge_set:
                x1, y1 = tx(nodes[a][0]), ty(nodes[a][1])
                x2, y2 = tx(nodes[b][0]), ty(nodes[b][1])
                f.write(f'  <line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}"/>\n')
            f.write('</g>\n')

        # Draw nodes
        f.write('<g fill="#ff4444">\n')
        for x, y, z in nodes:
            f.write(f'  <circle cx="{tx(x):.1f}" cy="{ty(y):.1f}" r="1.6"/>\n')
        f.write('</g>\n')

        # Map markers
        if markers:
            f.write('<g fill="#44ff66" font-family="sans-serif" font-size="13">\n')
            for name, x, y, z, fid in markers:
                px, py = tx(x), ty(y)
                f.write(f'  <path d="M{px:.1f},{py - 6:.1f} L{px - 5:.1f},{py + 4:.1f} '
                        f'L{px + 5:.1f},{py + 4:.1f} Z"/>\n')
                f.write(f'  <text x="{px + 7:.1f}" y="{py + 4:.1f}">{_xml_escape(name)}</text>\n')
            f.write('</g>\n')

        f.write('</svg>\n')

    print(f"Wrote SVG visualization to {svg_path}")


# ─── JSON output ─────────────────────────────────────────────────────────

def write_json(json_path, nodes, edges, bridges=None, markers=None, worldspace_edid=None,
               stats=None):
    """Write JSON debug output."""
    bridge_set = set((a, b) for a, b, w in (bridges or []))
    data = {
        'worldspace': worldspace_edid,
        'stats': stats or {},
        'nodes': [{'x': x, 'y': y, 'z': z} for x, y, z in nodes],
        'edges': [{'a': a, 'b': b, 'weight': w, 'bridge': (a, b) in bridge_set}
                  for a, b, w in edges],
        'map_markers': [{'name': name, 'x': round(x, 1), 'y': round(y, 1), 'z': round(z, 1),
                         'formid': f'{fid:08X}'} for name, x, y, z, fid in (markers or [])],
    }
    with open(json_path, 'w', encoding='utf-8') as f:
        json.dump(data, f, indent=1, ensure_ascii=False)
    print(f"Wrote JSON to {json_path}")


# ─── Main ────────────────────────────────────────────────────────────────

def find_esm():
    """Try to find Fallout3.esm in common locations."""
    candidates = [
        r"D:\SteamLibrary\steamapps\common\Fallout 3 goty\Data\Fallout3.esm",
        r"C:\Program Files (x86)\Steam\steamapps\common\Fallout 3 goty\Data\Fallout3.esm",
        r"C:\Program Files (x86)\Steam\steamapps\common\Fallout 3\Data\Fallout3.esm",
        r"C:\GOG Games\Fallout 3 GOTY\Data\Fallout3.esm",
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    return None


def resolve_output(path_arg, edid, ext, single, default_dir):
    """File path for one worldspace's output: explicit file if single, else dir/<EDID>.ext."""
    if path_arg and single and path_arg.lower().endswith(ext):
        return path_arg
    directory = path_arg if path_arg else default_dir
    os.makedirs(directory, exist_ok=True)
    return os.path.join(directory, f'{edid}{ext}')


def main():
    parser = argparse.ArgumentParser(
        description='Generate road waypoint graph (FalloutNVAccess FNVR v2 format) from Fallout3.esm')
    parser.add_argument('--esm', help='Path to ESM file (auto-detected for Fallout3.esm)')
    parser.add_argument('--master-esm',
                        help='Master ESM for road texture definitions (DLC processing)')
    parser.add_argument('--output', help='Output .dat file (single worldspace; default <EDID>.dat)')
    parser.add_argument('--output-dir', help='Output directory for <EDID>.dat files')
    parser.add_argument('--worldspace', action='append', dest='worldspaces',
                        help=f'Worldspace EDID to process (repeatable, default {DEFAULT_WORLDSPACE})')
    parser.add_argument('--all-worldspaces', action='store_true',
                        help='Generate one .dat per worldspace that yields a road graph')
    parser.add_argument('--road-edids-file', help='File with extra road texture EDIDs')
    parser.add_argument('--verbose', action='store_true', help='Print texture/mesh details')
    parser.add_argument('--svg', help='SVG visualization (file for one worldspace, else directory)')
    parser.add_argument('--json', help='JSON debug output (file for one worldspace, else directory)')
    parser.add_argument('--grid-size', type=int, default=512,
                        help='Spatial bin size in game units (default: 512)')
    parser.add_argument('--min-opacity', type=float, default=0.25,
                        help='Min visible road-texture weight to count as road (default: 0.25)')
    parser.add_argument('--raw-opacity', action='store_true',
                        help='FNV-original texture rule: ignore layers painted over a road texture')
    parser.add_argument('--subdivide', type=float, default=256.0,
                        help='Split navmesh triangles longer than this (default 256, 0 = off)')
    parser.add_argument('--no-preferred', action='store_true',
                        help='Do not treat navmesh "Preferred" triangles as road')
    parser.add_argument('--no-road-meshes', action='store_true',
                        help='Do not treat triangles under road mesh REFRs as road')
    parser.add_argument('--road-chunks', action='store_true',
                        help='Also treat RoadChunk*/RoadChunkCluster* debris meshes as road')
    parser.add_argument('--name-encoding', default='cp1250',
                        help='Code page of FULL names in the ESM (default cp1250 = Polish)')
    parser.add_argument('--bridge-dist', type=float, default=6144.0,
                        help='Max distance for bridging disconnected components (default: 6144)')
    parser.add_argument('--no-bridge', action='store_true',
                        help='Disable component bridging')
    args = parser.parse_args()

    esm_path = args.esm or find_esm()
    if not esm_path or not os.path.exists(esm_path):
        print("ERROR: ESM not found. Use --esm to specify path.")
        sys.exit(1)

    print(f"ESM: {esm_path}")
    if args.master_esm:
        print(f"Master ESM: {args.master_esm}")
    print(f"Grid size: {args.grid_size}, Min opacity: {args.min_opacity}, "
          f"subdivide: {args.subdivide}, preferred: {not args.no_preferred}, "
          f"road meshes: {not args.no_road_meshes}")
    print()

    esm = EsmFile(esm_path)

    # Load extra road EDIDs if specified
    extra_edids = None
    if args.road_edids_file:
        with open(args.road_edids_file) as f:
            extra_edids = [line.strip() for line in f if line.strip() and not line.startswith('#')]
        print(f"Loaded {len(extra_edids)} extra road texture EDIDs")

    # Pass 1: Find road textures
    print("=== Pass 1: Identifying road textures ===")
    ltex_map, road_formids = parse_ltex_records(esm, extra_edids)
    if args.master_esm:
        if not os.path.exists(args.master_esm):
            print(f"ERROR: Master ESM not found: {args.master_esm}")
            sys.exit(1)
        master = EsmFile(args.master_esm)
        master_ltex_map, master_road = parse_ltex_records(master, extra_edids)
        print(f"Master ESM: {len(master_ltex_map)} LTEX records, {len(master_road)} road textures")
        ltex_map = {**master_ltex_map, **ltex_map}
        road_formids = road_formids | master_road
    print(f"Found {len(ltex_map)} LTEX records, {len(road_formids)} road textures")

    if args.verbose:
        print("\nAll LTEX records (* = road):")
        for fid in sorted(ltex_map, key=lambda k: ltex_map[k].lower()):
            print(f"  {'*' if fid in road_formids else ' '} {fid:08X}: {ltex_map[fid]}")
        print()

    if not road_formids:
        print("WARNING: No road textures found (check ROAD_KEYWORDS or --road-edids-file)")

    # Road mesh base objects
    road_mesh_bases = {}
    if not args.no_road_meshes:
        road_mesh_bases = parse_road_mesh_bases(esm, include_chunks=args.road_chunks)
        if args.master_esm:
            road_mesh_bases = {**parse_road_mesh_bases(master, args.road_chunks), **road_mesh_bases}
        print(f"Road mesh base objects: {len(road_mesh_bases)}")
        if args.verbose:
            for fid in sorted(road_mesh_bases, key=lambda k: road_mesh_bases[k][0].lower()):
                edid, model, obnd = road_mesh_bases[fid]
                print(f"    {fid:08X}: {edid:36s} {model}  OBND={obnd}")

    # Worldspaces
    print("\n=== Parsing worldspace records ===")
    wrld_map = parse_wrld_records(esm)
    if args.master_esm:
        wrld_map = {**parse_wrld_records(master), **wrld_map}
    print(f"Found {len(wrld_map)} worldspaces")
    if args.all_worldspaces:
        worldspace_filter = set(wrld_map.keys())
    else:
        wanted = args.worldspaces or [DEFAULT_WORLDSPACE]
        edid_to_formid = {v.lower(): k for k, v in wrld_map.items()}
        worldspace_filter = set()
        for ws_edid in wanted:
            if ws_edid.lower() in edid_to_formid:
                worldspace_filter.add(edid_to_formid[ws_edid.lower()])
            else:
                print(f"WARNING: Worldspace '{ws_edid}' not found in ESM")
        if not worldspace_filter:
            print("ERROR: No valid worldspaces specified. Available: "
                  + ', '.join(sorted(wrld_map.values())))
            sys.exit(1)
    if args.verbose:
        for fid in sorted(worldspace_filter):
            print(f"  Processing worldspace: {wrld_map.get(fid, '???')} ({fid:08X})")

    # Pass 2: LAND textures, road mesh REFRs, map markers, NAVM record list
    print("\n=== Pass 2: Landscape textures, road meshes, map markers ===")
    ws_info, wrld_buf = parse_worldspaces(esm, worldspace_filter, road_formids, road_mesh_bases,
                                          args.min_opacity, args.name_encoding, args.raw_opacity)
    for ws_fid, info in ws_info.items():
        edid = wrld_map.get(ws_fid, f'{ws_fid:08X}')
        print(f"  {edid}: {len(info['road_positions'])} road texture positions in "
              f"{len(info['road_cells'])} cells, {len(info['meshes'])} road mesh REFRs, "
              f"{len(info['markers'])} map markers, {len(info['navm'])} NAVM records")
        if args.verbose and info['meshes']:
            per_base = defaultdict(int)
            for m in info['meshes']:
                per_base[m[6]] += 1
            for base, cnt in sorted(per_base.items(), key=lambda kv: -kv[1]):
                print(f"      {cnt:5d} x {road_mesh_bases[base][0]}")

    single = len(ws_info) == 1
    out_dir = args.output_dir or '.'
    generated = 0
    for ws_fid in sorted(ws_info.keys()):
        info = ws_info[ws_fid]
        edid = wrld_map.get(ws_fid, f'{ws_fid:08X}')

        # Pass 3: Cross-reference with navmesh
        print(f"\n=== Pass 3: Navmesh road triangles for {edid} ===")
        index = RoadIndex(info['road_positions'], info['meshes'])
        stats = {}
        road_centroids = parse_navm_road_triangles(
            wrld_buf, info['navm'], index, use_preferred=not args.no_preferred,
            subdivide=args.subdivide, stats=stats)
        print(f"Scanned {stats.get('navm', 0)} NAVM records, {stats.get('triangles', 0)} triangles; "
              f"{stats.get('road_triangles', 0)} road triangles -> {len(road_centroids)} samples "
              f"(preferred {stats.get('samples_preferred', 0)}, texture {stats.get('samples_texture', 0)}, "
              f"mesh {stats.get('samples_mesh', 0)})")
        if not road_centroids:
            print(f"  Skipping {edid}: no road navmesh triangles")
            continue

        print(f"\n--- Building graph for {edid} ---")
        nodes, edges = build_road_graph(road_centroids, grid_size=args.grid_size)
        if len(nodes) < 5:
            print(f"  Skipping {edid}: too few nodes ({len(nodes)})")
            continue

        bridges = []
        if not args.no_bridge:
            print(f"\n=== Bridging disconnected components (max dist: {args.bridge_dist}) ===")
            nodes, edges, bridges = bridge_components(nodes, edges, max_bridge_dist=args.bridge_dist)

        print("\n=== Writing output ===")
        if args.output and single:
            dat_path = args.output
        else:
            os.makedirs(out_dir, exist_ok=True)
            dat_path = os.path.join(out_dir, f'{edid}.dat')
        write_binary(dat_path, nodes, edges, args.grid_size, worldspace_edid=edid)

        summary = {
            'road_texture_positions': len(info['road_positions']),
            'road_mesh_refs': len(info['meshes']),
            'navm_records': stats.get('navm', 0),
            'navm_triangles': stats.get('triangles', 0),
            'road_triangles': stats.get('road_triangles', 0),
            'samples_preferred': stats.get('samples_preferred', 0),
            'samples_texture': stats.get('samples_texture', 0),
            'samples_mesh': stats.get('samples_mesh', 0),
            'nodes': len(nodes), 'edges': len(edges), 'bridges': len(bridges),
            'components': count_components(nodes, edges),
        }
        if args.svg:
            write_svg(resolve_output(args.svg, edid, '.svg', single, out_dir), nodes, edges,
                      road_centroids, bridges, info['markers'], title=f'{edid} road network')
        if args.json:
            write_json(resolve_output(args.json, edid, '.json', single, out_dir), nodes, edges,
                       bridges, info['markers'], edid, summary)
        generated += 1

    print(f"\nDone! Generated {generated} worldspace road network(s).")


if __name__ == '__main__':
    main()

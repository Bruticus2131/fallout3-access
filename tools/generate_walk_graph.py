"""
Generate WALK graphs for Fallout 3 worldspaces (Fallout3.esm): where a player
can walk, as a coarse graph over the game's whole navmesh, roads preferred.

Why not only roads
------------------
tools/generate_road_network.py (Alex's road graph, adapted) finds the roads,
and the FNV mod walks them only when the whole trip is at most 1.5x the
straight line. Fallout 3's roads are broken and winding: Vault 101 -> Megaton
by road is 1.58x, Megaton -> Rivet City 2.4x, so nearly every long walk was
rejected and went straight across, blind to rivers, cliffs and ruins.

This graph covers every navmesh triangle of a worldspace, so a route in it is
a way that exists on foot. Road pieces are cheaper to walk than open ground,
so a route keeps to a road while the road goes the right way and leaves it
when it turns away, as a person would. There is no detour rule any more: a
long way round is long because the short way does not exist.

How it is built
---------------
  1. Every NAVM of the worldspace: vertices (absolute world coordinates),
     triangles, and the links between triangles - inside one navmesh through
     NVTR's edge fields, across cells through NVEX (an edge whose external flag
     0x01/0x02/0x04 is set holds an index into NVEX: {uint32, NAVM form id,
     uint16 triangle}). Verified against Fallout3.esm 1.7: 5856 Wasteland
     navmeshes, NVEX entries 10 bytes, DATA {cell, nVerts, nTris, nExt, ...}.
  2. Each triangle is sampled (its centroid, or the sub-centroids of an n*n
     split for long triangles) and the samples are binned into GRID-unit
     squares. A triangle's piece in a square is joined with its neighbours'
     pieces in the same square; each joined group is one NODE (so a bridge
     over a road in the same square is a node of its own).
  3. Nodes are linked where their triangles touch (or one triangle spans both
     squares). A node stands at the sample nearest its group's middle, so it
     is always on the navmesh.
  4. A node is ROAD when its square holds enough road samples at its height
     (the road generator's rules: Preferred-flagged triangles, road
     textures, road mesh footprints). Edge weight = distance x 1.0 between
     road nodes, x OFFROAD otherwise, x WATER when either end is mostly water.

Output: 'FNVR' version 3 - the version-2 layout (header with the worldspace
EDID, nodes, edges, grid index) with the worldspace FORM ID in the header's
reserved field, followed by one flag byte per node (1 = road, 2 = water).

Version 4 is for the DLCs: the same, with a block after the EDID that names
the plugin the worldspace comes from and the plugins whose navmesh changes
are built in:
    u8 len, char module[len]                    e.g. "PointLookout.esm"
    u8 nOverlays, nOverlays x { u8 len, char name[len], u32 navmeshes }
The form id in the header is then the plugin's own (load-order byte 00): the
game numbers a DLC by where it sits in the load order, so the mod fills that
byte in when the game is running. A worldspace the DLCs change (Broken Steel
alone rebuilds 247 Wasteland navmeshes) gets a second file built with the
changes (--overlay), and the mod takes whichever matches the DLCs loaded.

Usage:
    python tools/generate_walk_graph.py --esm "<FO3>\\Data\\Fallout3.esm" \\
        --output-dir data/roadnet [--worldspace Wasteland] [--all-worldspaces]
  DLC worldspaces:
    python tools/generate_walk_graph.py --esm "<FO3>\\Data\\ThePitt.esm" \\
        --master "<FO3>\\Data\\Fallout3.esm" --all-worldspaces --output-dir data/roadnet
  Fallout3.esm worldspaces with the DLC navmesh changes built in:
    python tools/generate_walk_graph.py --esm "<FO3>\\Data\\Fallout3.esm" \\
        --worldspace Wasteland --overlay "<FO3>\\Data\\Anchorage.esm" ... --suffix +DLC
"""

import argparse
import math
import os
import struct
import sys
import time
from collections import defaultdict

import numpy as np
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import generate_road_network as rn  # noqa: E402  (ESM reader, road rules)

GRID = 512.0          # node square size
SAMPLE = 384.0        # triangles longer than this are split
OFFROAD = 1.35        # open ground costs this much more than road
WATER = 3.0           # wading
ROAD_MIN_SAMPLES = 2  # road samples a square needs to count as road
ROAD_Z = 256.0        # ...within this height of the node
MIN_COMPONENT = 3     # smaller islands are dropped
FLAG_ROAD, FLAG_WATER_NODE = 1, 2
TRI_WATER = 0x200


def load_navmeshes(buf, ws_filter, deleted=None):
    """{ws: [(navm_fid, verts ndarray Nx3, tris ndarray Mx8, nvex list)]}.
    `deleted` (a set) collects the form ids of deleted NAVM records (a plugin
    removing a navmesh of its master's)."""
    out = defaultdict(list)
    cur = None
    walker = rn.walk_records(buf, rn.HEADER_SIZE, len(buf))
    item = rn.advance(walker)
    while item is not None:
        if item[0] == 'GRUP':
            if item[2] == 1:
                ws = struct.unpack('<I', item[1])[0]
                if ws not in ws_filter:
                    item = rn.advance(walker, skip=True)
                    continue
                cur = ws
            item = rn.advance(walker)
            continue
        _, rec_type, flags, fid, off, size, stack = item
        item = rn.advance(walker)
        if cur is None or not stack or stack[0][1] != 1 or rec_type != b'NAVM':
            continue
        if flags & 0x20:
            if deleted is not None:
                deleted.add(fid)
            continue
        data = rn.record_data(buf, off, size, flags)
        if data is None:
            continue
        nvvx = nvtr = nvex = None
        for st, sd in rn.read_subrecords(data):
            if st == b'NVVX':
                nvvx = sd
            elif st == b'NVTR':
                nvtr = sd
            elif st == b'NVEX':
                nvex = sd
        if not nvvx or not nvtr:
            continue
        verts = np.frombuffer(nvvx[:len(nvvx) // 12 * 12], dtype='<f4').reshape(-1, 3)
        tris = np.frombuffer(nvtr[:len(nvtr) // 16 * 16], dtype='<i2').reshape(-1, 8)
        tris = tris.astype(np.int32)
        tris[:, 6] &= 0xFFFF
        tris[:, 7] &= 0xFFFF
        ext = []
        if nvex:
            for k in range(len(nvex) // 10):
                _u, nfid, t = struct.unpack_from('<IIH', nvex, k * 10)
                ext.append((nfid, t))
        out[cur].append((fid, verts, tris, ext))
    return out


def barycentrics(n):
    if n == 1:
        return np.array([[1 / 3, 1 / 3, 1 / 3]])
    w = []
    for i in range(n):
        for j in range(n - i):
            a, b = (i + 1 / 3) / n, (j + 1 / 3) / n
            w.append((a, b, 1 - a - b))
            if i + j < n - 1:
                a, b = (i + 2 / 3) / n, (j + 2 / 3) / n
                w.append((a, b, 1 - a - b))
    return np.array(w)


def build(navms, road_samples, verbose=False):
    t0 = time.time()
    # --- global triangles -------------------------------------------------------
    base = {}
    vbase = []
    nv = nt = 0
    for i, (fid, verts, tris, ext) in enumerate(navms):
        base[fid] = nt
        vbase.append(nv)
        nv += len(verts)
        nt += len(tris)
    V = np.concatenate([m[1] for m in navms]).astype(np.float64)
    tri_v = np.empty((nt, 3), dtype=np.int64)
    tri_flags = np.empty(nt, dtype=np.int32)
    pairs_a, pairs_b = [], []
    bad = ext_ok = ext_bad = 0
    for i, (fid, verts, tris, ext) in enumerate(navms):
        b = base[fid]
        n = len(tris)
        vi = tris[:, 0:3]
        valid = (vi >= 0) & (vi < len(verts))
        if not valid.all():
            bad += int((~valid.all(axis=1)).sum())
        tri_v[b:b + n] = np.clip(vi, 0, len(verts) - 1) + vbase[i]
        tri_flags[b:b + n] = tris[:, 6]
        for k in range(3):
            e = tris[:, 3 + k]
            external = (tris[:, 6] & (1 << k)) != 0
            internal = (~external) & (e >= 0) & (e < n)
            idx = np.nonzero(internal)[0]
            pairs_a.append(b + idx)
            pairs_b.append(b + e[idx])
            for t in np.nonzero(external & (e >= 0))[0]:
                ei = int(e[t])
                if ei >= len(ext):
                    ext_bad += 1
                    continue
                nfid, nt2 = ext[ei]
                if nfid in base and nt2 < (base_next(base, nfid, navms)):
                    pairs_a.append(np.array([b + t]))
                    pairs_b.append(np.array([base[nfid] + nt2]))
                    ext_ok += 1
                else:
                    ext_bad += 1
    A = np.concatenate(pairs_a).astype(np.int64)
    B = np.concatenate(pairs_b).astype(np.int64)
    if verbose:
        print(f"  {len(navms)} navmeshes, {nt} triangles, {len(A)} links "
              f"(cross-cell {ext_ok}, unresolved {ext_bad}, bad vertex refs {bad}) "
              f"[{time.time() - t0:.1f}s]")

    # --- samples ----------------------------------------------------------------
    P = V[tri_v]                                   # nt x 3 x 3
    e01 = np.hypot(P[:, 0, 0] - P[:, 1, 0], P[:, 0, 1] - P[:, 1, 1])
    e12 = np.hypot(P[:, 1, 0] - P[:, 2, 0], P[:, 1, 1] - P[:, 2, 1])
    e20 = np.hypot(P[:, 2, 0] - P[:, 0, 0], P[:, 2, 1] - P[:, 0, 1])
    nsub = np.maximum(1, np.ceil(np.maximum(np.maximum(e01, e12), e20) / SAMPLE)).astype(np.int64)
    nsub = np.minimum(nsub, 64)
    sx, sy, sz, st = [], [], [], []
    for n in np.unique(nsub):
        ids = np.nonzero(nsub == n)[0]
        W = barycentrics(int(n))                   # m x 3
        S = np.einsum('mk,tkc->tmc', W, P[ids])    # t x m x 3
        m = W.shape[0]
        sx.append(S[:, :, 0].ravel())
        sy.append(S[:, :, 1].ravel())
        sz.append(S[:, :, 2].ravel())
        st.append(np.repeat(ids, m))
    sx = np.concatenate(sx); sy = np.concatenate(sy); sz = np.concatenate(sz)
    st = np.concatenate(st)
    bx = np.floor(sx / GRID).astype(np.int64)
    by = np.floor(sy / GRID).astype(np.int64)
    # piece = (triangle, square)
    off = 1 << 19                                   # 20-bit fields: |square| < 2^19
    key = (st << 40) | ((bx + off) << 20) | (by + off)
    pkeys, piece_of_sample = np.unique(key, return_inverse=True)
    piece_tri = pkeys >> 40
    piece_bin = pkeys & ((1 << 40) - 1)
    npieces = len(pkeys)
    if verbose:
        print(f"  {len(sx)} samples, {npieces} pieces [{time.time() - t0:.1f}s]")

    # pieces of each triangle (CSR, pieces are sorted by triangle)
    first_piece = np.searchsorted(piece_tri, np.arange(nt + 1))
    cnt = first_piece[1:] - first_piece[:-1]

    # --- joins inside a square, links across squares ------------------------------
    ua, ub = [], []      # union (same square)
    la, lb = [], []      # link (different squares)
    # a) linked triangles: the common case has one piece each
    one = (cnt[A] == 1) & (cnt[B] == 1)
    pa = first_piece[A[one]]
    pb = first_piece[B[one]]
    same = piece_bin[pa] == piece_bin[pb]
    ua.append(pa[same]); ub.append(pb[same])
    la.append(pa[~same]); lb.append(pb[~same])
    for a, b in zip(A[~one], B[~one]):
        for p in range(first_piece[a], first_piece[a + 1]):
            for q in range(first_piece[b], first_piece[b + 1]):
                if piece_bin[p] == piece_bin[q]:
                    ua.append(np.array([p])); ub.append(np.array([q]))
                else:
                    la.append(np.array([p])); lb.append(np.array([q]))
    # b) one triangle over several squares
    for t in np.nonzero(cnt > 1)[0]:
        ps = np.arange(first_piece[t], first_piece[t + 1])
        for i in range(len(ps)):
            for j in range(i + 1, len(ps)):
                la.append(np.array([ps[i]])); lb.append(np.array([ps[j]]))
    ua = np.concatenate(ua); ub = np.concatenate(ub)
    la = np.concatenate([x for x in la if len(x)] or [np.zeros(0, np.int64)])
    lb = np.concatenate([x for x in lb if len(x)] or [np.zeros(0, np.int64)])
    g = coo_matrix((np.ones(len(ua)), (ua, ub)), shape=(npieces, npieces))
    nnodes, node_of_piece = connected_components(g, directed=False)
    node_of_piece = node_of_piece.astype(np.int64)
    node_of_sample = node_of_piece[piece_of_sample]
    if verbose:
        print(f"  {nnodes} raw nodes in {len(np.unique(piece_bin))} squares "
              f"[{time.time() - t0:.1f}s]")

    # --- node positions: the sample nearest the group's middle ---------------------
    cnts = np.bincount(node_of_sample, minlength=nnodes).astype(np.float64)
    mx = np.bincount(node_of_sample, sx, nnodes) / cnts
    my = np.bincount(node_of_sample, sy, nnodes) / cnts
    mz = np.bincount(node_of_sample, sz, nnodes) / cnts
    d2 = (sx - mx[node_of_sample]) ** 2 + (sy - my[node_of_sample]) ** 2 + \
         (sz - mz[node_of_sample]) ** 2
    order = np.lexsort((d2, node_of_sample))
    firsts = order[np.r_[0, np.nonzero(np.diff(node_of_sample[order]))[0] + 1]]
    nx_ = sx[firsts]; ny_ = sy[firsts]; nz_ = sz[firsts]
    water = np.bincount(node_of_sample, (tri_flags[st] & TRI_WATER) != 0, nnodes) / cnts

    # --- road squares -------------------------------------------------------------
    road = np.zeros(nnodes, dtype=bool)
    if len(road_samples):
        R = np.asarray(road_samples, dtype=np.float64)
        rb = defaultdict(list)
        for x, y, z in R:
            rb[(int(math.floor(x / GRID)), int(math.floor(y / GRID)))].append(z)
        nbx = np.floor(nx_ / GRID).astype(np.int64)
        nby = np.floor(ny_ / GRID).astype(np.int64)
        for i in range(nnodes):
            zs = rb.get((int(nbx[i]), int(nby[i])))
            if zs and sum(1 for z in zs if abs(z - nz_[i]) <= ROAD_Z) >= ROAD_MIN_SAMPLES:
                road[i] = True

    # --- edges --------------------------------------------------------------------
    ea = node_of_piece[la]; eb = node_of_piece[lb]
    keep = ea != eb
    ea, eb = ea[keep], eb[keep]
    lo = np.minimum(ea, eb); hi = np.maximum(ea, eb)
    ek = np.unique((lo << 32) | hi)
    ea = (ek >> 32).astype(np.int64); eb = (ek & 0xFFFFFFFF).astype(np.int64)

    # --- drop small islands ---------------------------------------------------------
    gg = coo_matrix((np.ones(len(ea)), (ea, eb)), shape=(nnodes, nnodes))
    ncomp, comp = connected_components(gg, directed=False)
    csize = np.bincount(comp, minlength=ncomp)
    alive = csize[comp] >= MIN_COMPONENT
    remap = -np.ones(nnodes, dtype=np.int64)
    remap[alive] = np.arange(int(alive.sum()))
    ke = alive[ea] & alive[eb]
    ea, eb = remap[ea[ke]], remap[eb[ke]]
    nx_, ny_, nz_ = nx_[alive], ny_[alive], nz_[alive]
    road, water = road[alive], water[alive]
    n = len(nx_)

    dist = np.sqrt((nx_[ea] - nx_[eb]) ** 2 + (ny_[ea] - ny_[eb]) ** 2 + (nz_[ea] - nz_[eb]) ** 2)
    factor = np.where(road[ea] & road[eb], 1.0, OFFROAD)
    wet = (water[ea] > 0.5) | (water[eb] > 0.5)
    factor = np.where(wet, WATER, factor)
    w = dist * factor
    flags = (road.astype(np.uint8) * FLAG_ROAD) | ((water > 0.5).astype(np.uint8) * FLAG_WATER_NODE)

    if n == 0:
        if verbose:
            print("  no nodes left after dropping islands")
        return [], [], flags
    gg = coo_matrix((np.ones(len(ea)), (ea, eb)), shape=(n, n))
    ncomp, comp = connected_components(gg, directed=False)
    csize = np.bincount(comp)
    if verbose:
        print(f"  {n} nodes ({int(road.sum())} road, {int((water > 0.5).sum())} water), "
              f"{len(ea)} edges, {ncomp} components, largest {csize.max() * 100.0 / n:.1f}% "
              f"(dropped {int((~alive).sum())} island nodes) [{time.time() - t0:.1f}s]")
    nodes = list(zip(nx_.tolist(), ny_.tolist(), nz_.tolist()))
    edges = list(zip(ea.tolist(), eb.tolist(), w.tolist()))
    return nodes, edges, flags


def base_next(base, fid, navms, _cache={}):
    """Triangle count of the navmesh `fid` (cached per run)."""
    key = id(navms)
    if _cache.get('key') != key:
        _cache.clear()
        _cache['key'] = key
        _cache['n'] = {m[0]: len(m[2]) for m in navms}
    return _cache['n'].get(fid, 0)


def write(path, nodes, edges, flags, ws_fid, edid, module=None, overlays=()):
    """module None: version 3 (Fallout3.esm, the full form id). Else version 4:
    the plugin's name, its own form id and the overlays [(name, navmeshes)]."""
    gx, gy, minx, miny, offs, cnts, lst = rn.build_grid_index(nodes, edges, GRID)
    maxx = minx + gx * GRID
    maxy = miny + gy * GRID
    eb = edid.encode('ascii', 'replace')[:32].ljust(32, b'\0')
    version = 3 if module is None else 4
    with open(path, 'wb') as f:
        f.write(b'FNVR')
        f.write(struct.pack('<IIIfffffI', version, len(nodes), len(edges), GRID, minx, miny, maxx,
                            maxy, ws_fid if version == 3 else ws_fid & 0xFFFFFF))
        f.write(eb)
        if version == 4:
            def name(n):
                b = n.encode('ascii')
                assert 0 < len(b) < 256
                return struct.pack('<B', len(b)) + b
            f.write(name(module))
            f.write(struct.pack('<B', len(overlays)))
            for o, count in overlays:
                f.write(name(o) + struct.pack('<I', count))
        f.write(np.asarray(nodes, dtype='<f4').tobytes())
        e = np.zeros(len(edges), dtype=[('a', '<u4'), ('b', '<u4'), ('w', '<f4')])
        if edges:
            arr = np.asarray(edges)
            e['a'] = arr[:, 0]; e['b'] = arr[:, 1]; e['w'] = arr[:, 2]
        f.write(e.tobytes())
        f.write(struct.pack('<II', gx, gy))
        f.write(np.asarray(offs, dtype='<u4').tobytes())
        f.write(np.asarray(cnts, dtype='<u4').tobytes())
        f.write(np.asarray(lst, dtype='<u4').tobytes())
        f.write(np.asarray(flags, dtype='u1').tobytes())
    print(f"  wrote {path} ({os.path.getsize(path):,} bytes)")


def masters_of(path):
    """The MAST list of a plugin's header."""
    with open(path, 'rb') as f:
        hdr = f.read(24)
        data = f.read(struct.unpack_from('<I', hdr, 4)[0])
    return [sd.rstrip(b'\0').decode('ascii', 'replace')
            for st, sd in rn.read_subrecords(data) if st == b'MAST']


def navm_fid(buf, rec):
    """The form id of a parse_worldspaces 'navm' entry (from its record header)."""
    return struct.unpack_from('<I', buf, rec[0] - 12)[0]


def road_sets(paths):
    """Road texture and road mesh form ids of these plugins, in the id space of
    the last one (a master's records keep their numbers there when it is listed
    first, as Fallout3.esm is in every DLC)."""
    ids, bases = set(), {}
    for p in paths:
        e = rn.EsmFile(p)
        ids |= rn.parse_ltex_records(e)[1]
        bases.update(rn.parse_road_mesh_bases(e))
    return ids, bases


class Overlay:
    """A later plugin whose navmesh changes go into a worldspace of the base."""

    def __init__(self, path, tag, base_module, tags, base_road):
        self.path = path
        self.name = os.path.basename(path)
        self.esm = rn.EsmFile(path)
        self.masters = masters_of(path)
        self.own = len(self.masters)
        self.tag = tag
        self.base_module = base_module.lower()
        self.tags = tags                  # earlier overlays: lower-case name -> tag
        self.buf = self.esm.load_group(b'WRLD')
        own_ids, own_bases = road_sets([path])
        self.road_ids = base_road[0] | own_ids
        self.road_bases = dict(base_road[1])
        self.road_bases.update(own_bases)

    def remap(self, fid):
        """The plugin's form id -> a number unique in the merged worldspace."""
        h = fid >> 24
        if h >= self.own:
            return (self.tag << 24) | (fid & 0xFFFFFF)
        m = self.masters[h].lower()
        if m == self.base_module:
            return fid & 0xFFFFFF
        if m in self.tags:
            return (self.tags[m] << 24) | (fid & 0xFFFFFF)
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    ap.add_argument('--esm', help='the plugin whose worldspaces are built (default Fallout3.esm)')
    ap.add_argument('--master', action='append', default=[],
                    help='a master of --esm, for its road textures and meshes (repeatable)')
    ap.add_argument('--overlay', action='append', default=[],
                    help='a later plugin whose navmesh changes are built in (repeatable, '
                         'in load order)')
    ap.add_argument('--suffix', default='', help='added to the output file names (e.g. +DLC)')
    ap.add_argument('--output-dir', default='.', help='where <EDID>.dat files go')
    ap.add_argument('--worldspace', action='append', dest='worldspaces',
                    help='worldspace EDID (repeatable, default Wasteland)')
    ap.add_argument('--all-worldspaces', action='store_true',
                    help='every worldspace the plugin itself defines')
    ap.add_argument('--min-nodes', type=int, default=40,
                    help='skip worldspaces with fewer nodes (default 40)')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    esm_path = args.esm or rn.find_esm()
    if not esm_path or not os.path.exists(esm_path):
        sys.exit('ESM not found, use --esm')
    module = os.path.basename(esm_path)
    own = len(masters_of(esm_path))
    esm = rn.EsmFile(esm_path)
    road_formids, road_mesh_bases = road_sets(args.master + [esm_path])
    wrld = rn.parse_wrld_records(esm)
    if args.all_worldspaces:
        wanted = {ws for ws in wrld if ws >> 24 == own}
    else:
        names = {v.lower(): k for k, v in wrld.items()}
        wanted = {names[w.lower()] for w in (args.worldspaces or ['Wasteland']) if w.lower() in names}
    if not wanted:
        sys.exit('no such worldspace; known: ' + ', '.join(sorted(wrld.values())))
    print(f"ESM {esm_path}: {len(wanted)} worldspace(s)")
    info, buf = rn.parse_worldspaces(esm, wanted, road_formids, road_mesh_bases)
    navms = load_navmeshes(buf, wanted)
    overlays, tags = [], {}
    for i, p in enumerate(args.overlay):
        ov = Overlay(p, 0x80 + i, module, dict(tags), (road_formids, road_mesh_bases))
        tags[ov.name.lower()] = ov.tag
        overlays.append(ov)
    os.makedirs(args.output_dir, exist_ok=True)
    for ws in sorted(wanted):
        edid = wrld.get(ws, '%08X' % ws)
        if not navms.get(ws):
            continue
        print(f"\n{edid} ({ws:08X})")
        final = {m[0]: m for m in navms[ws]}
        road_pos = set(info[ws]['road_positions'])
        meshes = list(info[ws]['meshes'])
        recs = {navm_fid(buf, r): (buf, r) for r in info[ws]['navm']}
        used = []
        for ov in overlays:
            if ov.buf is None:
                continue
            gone = set()
            got = load_navmeshes(ov.buf, {ws}, gone).get(ws, [])
            if not got and not gone:
                continue
            used.append((ov.name, len(got) + len(gone)))
            for fid in gone:
                k = ov.remap(fid)
                final.pop(k, None)
                recs.pop(k, None)
            for fid, verts, tris, ext in got:
                k = ov.remap(fid)
                if k is None:
                    continue
                ext2 = []
                for nfid, t in ext:
                    nk = ov.remap(nfid)
                    ext2.append((nk if nk is not None else 0xFFFFFFFF, t))
                final[k] = (k, verts, tris, ext2)
            oinfo, obuf = rn.parse_worldspaces(ov.esm, {ws}, ov.road_ids, ov.road_bases)
            if ws in oinfo:
                road_pos |= oinfo[ws]['road_positions']
                meshes += oinfo[ws]['meshes']
                for r in oinfo[ws]['navm']:
                    k = ov.remap(navm_fid(obuf, r))
                    if k is not None:
                        recs[k] = (obuf, r)
            if args.verbose:
                print(f"  {ov.name}: {len(got)} navmeshes changed or added, {len(gone)} removed")
        if args.overlay and not used:
            print("  no overlay changes this worldspace - skipped")
            continue
        index = rn.RoadIndex(road_pos, meshes)
        road = []
        by_buf = defaultdict(list)
        for bb, r in recs.values():
            by_buf[id(bb)].append((bb, r))
        for lst in by_buf.values():
            road += rn.parse_navm_road_triangles(lst[0][0], [r for _, r in lst], index)
        nodes, edges, flags = build(list(final.values()), road, args.verbose)
        if len(nodes) < args.min_nodes:
            print(f"  skipped: {len(nodes)} nodes")
            continue
        v4 = own > 0 or bool(used)
        write(os.path.join(args.output_dir, edid + args.suffix + '.dat'), nodes, edges, flags, ws,
              edid, module if v4 else None, used)


if __name__ == '__main__':
    main()

# A map exported as OBJ + MTL + PNG textures (Wumba's Wigwam's export, e.g. Cave of Horror) built into Conker's Bad Fur Day's level
# format (model bank 4: an entry table, then one chunk: a 10-word header, its vertices from 0x28 (segment 1), its display list, then one
# u32 surface value per triangle in the display list's order, the collision: the visible triangles are the solid ones), packed as the
# game's entries are (4-byte big-endian size, then raw deflate), with its textures as the game's CI8 texture files (texels, then a
# 256-colour RGBA5551 palette) under new ids, for host/src/mp_characters.cpp to hand out (func_1510D0EC). The same layout as Pirate
# Lagoon's (host/assets/lagoon).
#   python3 cave_level.py <folder with the .obj, .mtl and .png> <out folder> [--scale=1.5] [--tex=0x2100] [--surface=0x5]
import sys, os, struct, zlib, glob
import numpy as np
from PIL import Image

src, out = sys.argv[1], sys.argv[2]
opt = lambda name, d: next((a.split('=', 1)[1] for a in sys.argv if a.startswith('--' + name + '=')), d)
SCALE = float(opt('scale', '1.5'))
TEX_BASE = int(opt('tex', '0x2100'), 0)
SURFACE = int(opt('surface', '0x5'), 0)
MAX_EDGE = float(opt('edge', '100000'))     # big triangles split until no side is longer (the game's collision lets players fall through big ones)
MAX_EDGE_OTHER = float(opt('edge_other', '100000'))   # (walls and ceilings: coarser, to keep the level within the arena's room)
BRIGHT = float(opt('gamma', '0.6'))      # textures lifted (out = in ^ gamma): the map's own lighting was baked in vertex colours
os.makedirs(out, exist_ok=True)

objf = glob.glob(os.path.join(src, '*.obj'))[0]
mtlf = glob.glob(os.path.join(src, '*.mtl'))[0]
mtl = {}
cur = None
for line in open(mtlf, errors='ignore'):
    w = line.split()
    if not w:
        continue
    if w[0] == 'newmtl':
        cur = w[1]; mtl[cur] = None
    elif w[0] == 'map_Kd' and cur:
        mtl[cur] = os.path.join(src, ' '.join(w[1:]))
V, VT, faces = [], [], []
cur = None
for line in open(objf, errors='ignore'):
    w = line.split()
    if not w:
        continue
    if w[0] == 'v':
        V.append([float(x) for x in w[1:4]])
    elif w[0] == 'vt':
        VT.append([float(w[1]), float(w[2])])
    elif w[0] == 'usemtl':
        cur = w[1]
    elif w[0] == 'f':
        idx = []
        for p in w[1:]:
            a = p.split('/')
            vi = int(a[0]); ti = int(a[1]) if len(a) > 1 and a[1] else 0
            idx.append((vi - 1 if vi > 0 else len(V) + vi, (ti - 1 if ti > 0 else len(VT) + ti) if ti else -1))
        for k in range(1, len(idx) - 1):
            faces.append((cur, idx[0], idx[k], idx[k + 1]))
V = np.array(V) * SCALE
# (--decimate=triangles: a map too big for the arena's room simplified by edge collapses chosen to change its shape least (quadric
# error, pyfqmr), each material on its own with its edges kept (its borders with the other materials, its texture seams, its open
# edges), so no cracks open between them and its textures stay put; the new corners' UVs from the nearest point of the material's
# own triangles before. Better kept than --cluster's grid, which moves every corner)
DECIMATE = int(opt('decimate', '0'))
if DECIMATE > 0 and len(faces) > DECIMATE:
    import pyfqmr
    total = len(faces)
    # (the whole map as one mesh, its corners joined by place, simplified; its open edges (the map's outline) kept)
    key, P, T, src = {}, [], [], []
    for n_, f in enumerate(faces):
        tri = []
        for vi, ti in f[1:]:
            k = tuple(np.round(V[vi], 3))
            if k not in key:
                key[k] = len(P); P.append(V[vi])
            tri.append(key[k])
        if len(set(tri)) == 3:
            T.append(tri); src.append(n_)
    P = np.array(P, float); T = np.array(T, np.int32)
    sim = pyfqmr.Simplify()
    sim.setMesh(P, T)
    sim.simplify_mesh(target_count=DECIMATE, aggressiveness=7, preserve_border=bool(int(opt('keep_outline', '1'))), verbose=False)
    NP, NT, _ = sim.getMesh()
    # (each new triangle takes the material and the texture mapping of the old triangle nearest its middle: its corners' UVs from that
    # triangle's own mapping (carried on past its edges where they reach out), so a triangle never spans a texture seam)
    OA = np.array([V[f[1][0]] for f in faces]); OB = np.array([V[f[2][0]] for f in faces]); OC = np.array([V[f[3][0]] for f in faces])
    e0, e1 = OB - OA, OC - OA
    d00 = np.einsum('ij,ij->i', e0, e0); d01 = np.einsum('ij,ij->i', e0, e1); d11 = np.einsum('ij,ij->i', e1, e1)
    den = d00 * d11 - d01 * d01
    good = np.abs(den) > 1e-9
    den = np.where(good, den, 1.0)
    def bary(p, idx):
        v2 = p - OA[idx]
        d20 = np.einsum('ij,ij->i', v2, e0[idx]); d21 = np.einsum('ij,ij->i', v2, e1[idx])
        bv = (d11[idx] * d20 - d01[idx] * d21) / den[idx]; bw = (d00[idx] * d21 - d01[idx] * d20) / den[idx]
        return bv, bw
    allidx = np.arange(len(faces))
    Vl = [p for p in V]
    VTl = [t for t in VT]
    out_f = []
    shared = {}
    mat_of = np.array([hash(f[0]) for f in faces])
    by_mat = {}
    for n_, f in enumerate(faces):
        by_mat.setdefault(f[0], []).append(n_)
    by_mat = {m: np.array(v) for m, v in by_mat.items()}
    def corner_uv(c, m):
        k = (c, m)
        if k in shared:
            return shared[k]
        idx = by_mat[m]
        p = NP[c]
        bv, bw = bary(p, idx)
        cv, cw = np.clip(bv, 0, 1), np.clip(bw, 0, 1)
        sm = cv + cw
        sc = np.where(sm > 1, 1.0 / np.maximum(sm, 1e-12), 1.0)
        cv, cw = cv * sc, cw * sc
        q = OA[idx] + cv[:, None] * e0[idx] + cw[:, None] * e1[idx]
        j = int(np.argmin(np.linalg.norm(q - p, axis=1) + np.where(good[idx], 0.0, 1e9)))
        f = faces[idx[j]]
        uvj = [np.array(VT[cc[1]]) if cc[1] >= 0 else np.zeros(2) for cc in f[1:]]
        t_uv = (1 - cv[j] - cw[j]) * uvj[0] + cv[j] * uvj[1] + cw[j] * uvj[2]
        Vl.append(p); VTl.append(tuple(t_uv))
        shared[k] = (t_uv, (len(Vl) - 1, len(VTl) - 1))
        return shared[k]
    n_shared = [0]
    for tri in NT:
        cpt = NP[tri].mean(0)
        bv, bw = bary(cpt, allidx)
        cv, cw = np.clip(bv, 0, 1), np.clip(bw, 0, 1)
        sm = cv + cw
        sc = np.where(sm > 1, 1.0 / np.maximum(sm, 1e-12), 1.0)
        cv, cw = cv * sc, cw * sc
        q = OA + cv[:, None] * e0 + cw[:, None] * e1
        dd = np.linalg.norm(q - cpt, axis=1) + np.where(good, 0.0, 1e9)
        o = int(np.argmin(dd))
        f = faces[o]
        uv = [np.array(VT[c[1]]) if c[1] >= 0 else np.zeros(2) for c in f[1:]]
        aff = []
        for c in tri:
            pv, pw = bary(NP[c], np.array([o]))
            pu_ = 1 - pv[0] - pw[0]
            aff.append(pu_ * uv[0] + pv[0] * uv[1] + pw[0] * uv[2])
        # (the corners' UVs where they're shared: each corner's own for this material, from the nearest point of the material's
        # triangles; taken when they agree with this triangle's own mapping (not across a texture seam), else its own mapping's)
        own = [corner_uv(int(c), f[0]) for c in tri]
        # (shared unless the corners' own UVs stretch the texture across the triangle unlike its own mapping does: a texture seam)
        eo = [own[(k + 1) % 3][0] - own[k][0] for k in range(3)]
        ea = [aff[(k + 1) % 3] - aff[k] for k in range(3)]
        if all(np.max(np.abs(eo[k] - ea[k])) < 0.25 for k in range(3)):
            corners = [own[k][1] for k in range(3)]
            n_shared[0] += 1
        else:
            corners = []
            for k, c in enumerate(tri):
                Vl.append(NP[c]); VTl.append(tuple(aff[k]))
                corners.append((len(Vl) - 1, len(VTl) - 1))
        out_f.append((f[0],) + tuple(corners))
    V = np.array(Vl); VT = VTl
    print('decimated:', total, 'triangles to', len(out_f), '(%d with shared corners)' % n_shared[0])
    faces = out_f
# (--cluster=size: a map too big for the arena's room simplified: each material's corners within the same cell of that size (in the
# OBJ's own units) moved to their middle, the triangles left with no area dropped; their UVs as they were)
CLUSTER = float(opt('cluster', '0'))
# (--cluster_join=1: the cells shared by every material, so the corners of two materials in one cell go to the same place and no
# crack opens between them; the default before was each material its own)
GLOBAL_CELLS = bool(int(opt('cluster_join', '1')))
if CLUSTER > 0:
    cell = CLUSTER * SCALE
    keys = {}
    sums = {}
    for f in faces:
        for c in f[1:]:
            k = (f[0] if not GLOBAL_CELLS else None,) + tuple(np.floor(V[c[0]] / cell).astype(int))
            keys[(f[0], c[0])] = k
            sums.setdefault(k, [np.zeros(3), 0, set()])
            if c[0] not in sums[k][2]:
                sums[k][2].add(c[0]); sums[k][0] += V[c[0]]; sums[k][1] += 1
    Vl = list(V); newi = {}
    for k, (sm, n, _) in sums.items():
        newi[k] = len(Vl); Vl.append(sm / n)
    V = np.array(Vl)
    out_f, seen = [], set()
    for f in faces:
        cs = [(newi[keys[(f[0], c[0])]], c[1]) for c in f[1:]]
        ids = [c[0] for c in cs]
        if len(set(ids)) < 3:
            continue
        sig = (f[0],) + tuple(sorted(ids))
        if sig in seen:
            continue
        seen.add(sig)
        out_f.append((f[0],) + tuple(cs))
    print('simplified:', len(faces), 'triangles to', len(out_f))
    faces = out_f
print('vertices', len(V), 'triangles', len(faces), 'materials', len(mtl))
if opt('dump', ''):
    np.save(opt('dump', ''), np.array([[V[c[0]] for c in f[1:]] for f in faces]))


# ---- textures: each material's PNG to CI8, at most 2048 texels (64 x 32), its sides powers of two
def pow2(n):
    p = 1
    while p < n:
        p *= 2
    return p
def to_ci8(img):
    img = img.convert('RGBA')
    w, h = pow2(img.size[0]), pow2(img.size[1])
    while w * h > 2048:
        if w >= h: w //= 2
        else: h //= 2
    img = img.resize((w, h), Image.LANCZOS)
    a = np.asarray(img).copy()
    a[:, :, :3] = (255.0 * (a[:, :, :3] / 255.0) ** BRIGHT).round().astype(np.uint8)
    img = Image.fromarray(a, 'RGBA')
    q = img.convert('RGB').quantize(colors=255, method=Image.MEDIANCUT, dither=Image.NONE)
    idx = np.asarray(q).astype(np.uint8) + 1
    idx[a[:, :, 3] < 128] = 0
    pal = (q.getpalette() or [])[:255 * 3]; pal += [0] * (255 * 3 - len(pal))
    c = lambda r, g, b: ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1
    pal16 = [0] + [c(pal[3 * i], pal[3 * i + 1], pal[3 * i + 2]) for i in range(255)]
    # (0.11.4: see-through texels (index 0) cut out by their material's render mode, below; index 0's colour the texture's average, no
    # alpha bit, so filtering at a cut edge blends towards the texture's own colour, not black (tools/cave/cutout.py does the same to
    # maps built before))
    used_ = idx[idx > 0]
    if (idx == 0).any() and len(used_):
        cnt = np.bincount(used_.ravel(), minlength=256)
        rgb = np.array([[pal[3 * (i - 1)], pal[3 * (i - 1) + 1], pal[3 * (i - 1) + 2]] if i > 0 else [0, 0, 0] for i in range(256)], float)
        avg = (rgb * cnt[:, None]).sum(0) / max(1, cnt.sum())
        pal16[0] = c(*[int(round(v)) for v in avg]) & ~1
    px = idx.copy()
    for y in range(1, h, 2):   # (odd rows' 32-bit words swapped, as TMEM has them)
        px[y] = px[y].reshape(-1, 2, 4)[:, ::-1, :].reshape(-1)
    return w, h, px.tobytes() + b''.join(struct.pack('>H', v) for v in pal16)
used = []
for f in faces:
    if f[0] not in used:
        used.append(f[0])
tex = {}
see_through = {}
for m in used:
    path = mtl.get(m)
    if not path or not os.path.exists(path):
        continue
    tid = TEX_BASE + len(tex)
    w, h, data = to_ci8(Image.open(path))
    tex[m] = (tid, w, h)
    see_through[m] = 0 in data[:w * h]
    open(os.path.join(out, 'tex_%04X.bin' % tid), 'wb').write(data)
print('textures', len(tex))

# ---- the chunk
def log2(n):
    return n.bit_length() - 1
verts, dl, surf = [], [], []
def cmd(w0, w1):
    dl.append(struct.pack('>II', w0 & 0xFFFFFFFF, w1 & 0xFFFFFFFF))
cmd(0xD7000000, 0xFFFFFFFF)
cmd(0xE7000000, 0)
cmd(0xFC457E8A, 0x55FDF8FC)
cmd(0xEF082C3F, 0x00552230)
cmd(0xD9FAFDFF, 0)
cmd(0xD9FFF9FF, 0x00200005)   # shaded (white vertex colours; without G_SHADE all black), smooth, z-buffered; neither side culled
# a triangle split into k x k smaller ones (k from its longest side), positions and UVs between its corners
def split(f):
    P = [V[c[0]] for c in f[1:]]
    U = [np.array(VT[c[1]]) if c[1] >= 0 else np.zeros(2) for c in f[1:]]
    e = max(np.linalg.norm(P[0] - P[1]), np.linalg.norm(P[1] - P[2]), np.linalg.norm(P[2] - P[0]))
    nrm = np.cross(P[1] - P[0], P[2] - P[0]); floor = abs(nrm[1]) > 0.6 * max(np.linalg.norm(nrm), 1e-9)
    k = max(1, int(np.ceil(e / (MAX_EDGE if floor else MAX_EDGE_OTHER))))
    def at(i, j):   # i along a->b, j along a->c, in k steps
        a, b = i / k, j / k
        return P[0] + (P[1] - P[0]) * a + (P[2] - P[0]) * b, U[0] + (U[1] - U[0]) * a + (U[2] - U[0]) * b
    out = []
    for i in range(k):
        for j in range(k - i):
            out.append((at(i, j), at(i + 1, j), at(i, j + 1)))
            if i + j < k - 1:
                out.append((at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)))
    return out
ntris = 0
def local_order(tris):
    # (the triangles in an order that keeps neighbours together: each next one, where it can be, one sharing a corner (place and UV)
    # with the last few, so the batches of 30 vertices share more of them)
    keyof = lambda c: (tuple(np.round(V[c[0]], 1)), tuple(np.round(VT[c[1]], 4)) if c[1] >= 0 else None)
    corners = [[keyof(c) for c in f[1:]] for f in tris]
    by = {}
    for n_, ks in enumerate(corners):
        for k in ks:
            by.setdefault(k, []).append(n_)
    done = [False] * len(tris)
    out = []
    recent = []
    for seed in range(len(tris)):
        if done[seed]:
            continue
        stack = [seed]
        while stack:
            t = stack.pop()
            if done[t]:
                continue
            done[t] = True
            out.append(tris[t])
            nxt = []
            for k in corners[t]:
                for u in by[k]:
                    if not done[u]:
                        nxt.append(u)
            # (the ones sharing the most corners with this one last, so they're taken next)
            nxt.sort(key=lambda u: sum(1 for k in corners[u] if k in corners[t]))
            stack.extend(nxt)
    return out
for m in used:
    tris = local_order([f for f in faces if f[0] == m])
    if m in tex:
        tid, w, h = tex[m]
        line = (w + 7) // 8
        cmd(0xE7000000, 0)
        cmd(0xFD500000, tid)                                   # the texture, by id (CI8 as 16-bit for the load)
        cmd(0xF5500000, 0x07000000)
        cmd(0xE6000000, 0)
        cmd(0xF3000000, 0x07000000 | (min(2047, (w * h + 1) // 2 - 1) << 12))   # (dxt 0: the odd rows are swapped already)
        cmd(0xE7000000, 0)
        cmd(0xF5480000 | (line << 9), (log2(h) << 14) | (log2(w) << 4))   # tile 0: CI8, wrapping both ways
        cmd(0xF2000000, (((w - 1) << 2) << 12) | ((h - 1) << 2))
        cmd(0xFD100000, 0x00400000 | tid)                      # its palette
        cmd(0xF5600100, 0x06000000)
        cmd(0xE6000000, 0)
        cmd(0xF0000000, 0x063FC000)
        cmd(0xE7000000, 0)
        cmd(0xD7000002, 0xFFFFFFFF)
        cmd(0xFC121824, 0x5531FEFF)
        # (its render mode: opaque, z-buffered; a texture with see-through texels with CVG_X_ALPHA too (0x1000): its coverage times the
        # texel's alpha, so those aren't drawn (they were drawn black))
        cmd(0xEF08AC3F, 0x00553230 if see_through.get(m) else 0x00552230)
    else:
        w = h = 32
    # the pieces in batches of up to 30 vertices, shared where position and UV agree
    batch_v, batch_key, batch_t = [], {}, []
    def flush():
        if not batch_t:
            return
        base = len(verts)
        verts.extend(batch_v)
        n = len(batch_v)
        cmd(0x01000000 | (n << 12) | ((n * 2) & 0xFF), 0x01000000 | (base * 16))
        for i in range(0, len(batch_t), 2):
            a = batch_t[i]
            if i + 1 < len(batch_t):
                b = batch_t[i + 1]
                cmd(0x06000000 | (a[0] * 2 << 16) | (a[1] * 2 << 8) | a[2] * 2, (b[0] * 2 << 16) | (b[1] * 2 << 8) | b[2] * 2)
            else:
                cmd(0x05000000 | (a[0] * 2 << 16) | (a[1] * 2 << 8) | a[2] * 2, 0)
            surf.extend([SURFACE] * (2 if i + 1 < len(batch_t) else 1))
        batch_v.clear(); batch_key.clear(); batch_t.clear()
    for f in tris:
        pieces = split(f)
        # (one UV shift for all of a triangle's pieces, so they share; whole tiles, to keep the numbers small)
        du = np.floor(min(p[1][0] for q in pieces for p in q)); dv = np.floor(min(-p[1][1] for q in pieces for p in q))
        # (no shift where the UVs fit as they are: the triangles then share their corners)
        us = [p[1][0] for q in pieces for p in q]; vs = [-p[1][1] for q in pieces for p in q]
        if max(abs(min(us)), abs(max(us))) * w * 32 < 32000 and max(abs(min(vs)), abs(max(vs))) * h * 32 < 32000:
            du = dv = 0.0
        for q in pieces:
            keys = []
            for (x, y, z), u in q:
                s_ = max(-32768, min(32767, int(round((u[0] - du) * w * 32)))); t_ = max(-32768, min(32767, int(round((-u[1] - dv) * h * 32))))
                keys.append((int(round(x)), int(round(y)), int(round(z)), s_, t_))
            new = len({k_ for k_ in keys if k_ not in batch_key})
            if len(batch_v) + new > 30:
                flush()
            t = []
            for k_ in keys:
                if k_ not in batch_key:
                    batch_key[k_] = len(batch_v)
                    batch_v.append(struct.pack('>hhhHhhBBBB', k_[0], k_[1], k_[2], 0, k_[3], k_[4], 255, 255, 255, 255))
                t.append(batch_key[k_])
            batch_t.append(t)
            ntris += 1
    flush()
# a floor under the whole map, just below its lowest (--basement=0 for none): the game draws a level only while the camera has a
# floor under it, and a camera pulled back through a wall into the space outside had none (the map went black). Not shown from
# inside: below the floors.
if float(opt('basement', '1')) > 0:
    lo_, hi_ = V.min(0), V.max(0)
    yb = lo_[1] - 20.0
    q = [(lo_[0], yb, lo_[2]), (hi_[0], yb, lo_[2]), (hi_[0], yb, hi_[2]), (lo_[0], yb, hi_[2])]
    base = len(verts)
    for (x, y, z) in q:
        verts.append(struct.pack('>hhhHhhBBBB', int(round(x)), int(round(y)), int(round(z)), 0, 0, 0, 40, 40, 40, 255))
    cmd(0x01000000 | (4 << 12) | (8 & 0xFF), 0x01000000 | (base * 16))
    cmd(0x06000000 | (0 << 16) | (2 << 8) | 4, (0 << 16) | (4 << 8) | 6)
    surf.extend([SURFACE, SURFACE])
    ntris += 2
cmd(0xDF000000, 0)
print('pieces', ntris)

vbytes = b''.join(verts)
dl_bytes = b''.join(dl)
coll = b''.join(struct.pack('>I', v) for v in surf)
dl_off = 0x28 + len(vbytes)
coll_off = dl_off + len(dl_bytes)
header = struct.pack('>10I', dl_off, len(dl_bytes), 0, 0, coll_off, len(coll), 0, 0, 0, 0x80000000)
chunk = header + vbytes + dl_bytes + coll
entry = struct.pack('>II', 8, 0x80000000 | len(chunk)) + chunk
open(os.path.join(out, 'cave_level.bin'), 'wb').write(entry)
c = zlib.compressobj(9, zlib.DEFLATED, -15)
open(os.path.join(out, 'cave.rzip'), 'wb').write(struct.pack('>I', len(entry)) + c.compress(entry) + c.flush())
print('level', len(entry), 'bytes:', len(verts), 'vertices,', len(surf), 'triangles; floor at', round(V[:, 1].min()), 'spans',
      [round(V[:, i].min()) for i in range(3)], [round(V[:, i].max()) for i in range(3)])

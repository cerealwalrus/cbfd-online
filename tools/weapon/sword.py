# The multiplayer sword replaced by a mesh (OBJ + MTL + PNG): its pickup (bank 3 entry 0x52, the prop's model) and its held model (bank 9
# entry 0x3A, on bone 9), each rebuilt in its entry's own layout from the ROM's (on this PC), its geometry ours: the blade along +z, the
# grip at the original's. Textures CI8 under ids 0x23C0 on (handed out by mp_characters.cpp, assets/sword/tex_XXXX.bin).
#   python3 -I tools/weapon/sword.py <folder with the .obj> [--tex=0x23C0]
import sys, os, struct, zlib, glob
import numpy as np
from PIL import Image
src = sys.argv[1]
opt = lambda k, d: next((a.split('=', 1)[1] for a in sys.argv if a.startswith('--' + k + '=')), d)
TEX = int(opt('tex', '0x23C0'), 0)
out = os.path.join('host', 'assets', 'sword'); os.makedirs(out, exist_ok=True)
r = open('conker/baserom.us.z64', 'rb').read()
def ent(B, e):
    off, fl = struct.unpack('>II', r[B + 8 * e:B + 8 * e + 8]); blk = r[B + off:B + off + (fl & 0x0FFFFFFF)]
    return zlib.decompressobj(wbits=-15).decompress(blk[4:])
pickup0 = ent(0xF8F278, 0x52); held0 = ent(0x1204780, 0x3A)
# ---- the mesh
objf = glob.glob(os.path.join(src, '*.obj'))[0]; mtlf = glob.glob(os.path.join(src, '*.mtl'))
mtl = {}; cur = None
for line in open(mtlf[0], errors='ignore') if mtlf else []:
    w = line.split()
    if w and w[0] == 'newmtl': cur = w[1]; mtl[cur] = None
    elif w and w[0] == 'map_Kd' and cur: mtl[cur] = os.path.join(src, os.path.basename(' '.join(w[1:]).replace('\\', '/')))
V, VT, F = [], [], []; cur = None
for line in open(objf, errors='ignore'):
    w = line.split()
    if not w: continue
    if w[0] == 'v': V.append([float(x) for x in w[1:4]])
    elif w[0] == 'vt': VT.append([float(w[1]), float(w[2])])
    elif w[0] == 'usemtl': cur = w[1]
    elif w[0] == 'f':
        idx = []
        for p in w[1:]:
            a = p.split('/'); vi = int(a[0]) - 1; ti = int(a[1]) - 1 if len(a) > 1 and a[1] else -1
            idx.append((vi, ti))
        for k in range(1, len(idx) - 1): F.append((cur, idx[0], idx[k], idx[k + 1]))
V = np.array(V)
# (its longest side is the blade, its lowest end there the pommel: the blade along +z, flat across y, thin in x)
ax = int(np.argmax(V.max(0) - V.min(0))); rest = [i for i in range(3) if i != ax]
wide = rest[int(np.argmax([(V.max(0) - V.min(0))[i] for i in rest]))]; thin = [i for i in rest if i != wide][0]
def place(z0, length):
    s = length / (V[:, ax].max() - V[:, ax].min())
    P = np.zeros_like(V)
    P[:, 2] = (V[:, ax] - V[:, ax].min()) * s + z0
    P[:, 1] = (V[:, wide] - (V[:, wide].max() + V[:, wide].min()) / 2) * s
    P[:, 0] = (V[:, thin] - (V[:, thin].max() + V[:, thin].min()) / 2) * s
    return P
# ---- textures (as obj_character.py's: CI8, odd rows swapped, 256-colour palette)
def pow2(n):
    p = 1
    while p < n: p *= 2
    return p
def to_ci8(img):
    img = img.convert('RGBA')
    w, h = max(8, min(64, pow2(img.size[0]))), max(8, min(64, pow2(img.size[1])))
    while w * h > 2048:
        if w >= h: w //= 2
        else: h //= 2
    img = img.resize((w, h), Image.LANCZOS); a = np.asarray(img)
    q = img.convert('RGB').quantize(colors=255, method=Image.MEDIANCUT, dither=Image.NONE)
    idx = np.asarray(q).astype(np.uint8) + 1; idx[a[:, :, 3] < 128] = 0
    pal = (q.getpalette() or [])[:255 * 3]; pal += [0] * (255 * 3 - len(pal))
    c4 = lambda r_, g, b_: ((r_ >> 3) << 11) | ((g >> 3) << 6) | ((b_ >> 3) << 1) | 1
    pal16 = [0] + [c4(pal[3 * i], pal[3 * i + 1], pal[3 * i + 2]) for i in range(255)]
    px = idx.copy()
    for y in range(1, h, 2): px[y] = px[y].reshape(-1, 2, 4)[:, ::-1, :].reshape(-1)
    return w, h, px.tobytes() + b''.join(struct.pack('>H', v) for v in pal16)
used = []
for f in F:
    if f[0] not in used: used.append(f[0])
tex = {}
for m in used:
    p = mtl.get(m)
    if not p or not os.path.exists(p): continue
    tid = TEX + len(tex); w, h, data = to_ci8(Image.open(p)); tex[m] = (tid, w, h)
    open(os.path.join(out, 'tex_%04X.bin' % tid), 'wb').write(data)
def log2(n): return n.bit_length() - 1
# ---- a display list and vertex block for one placement
def build(P, head_cmds, per_tex_tail):
    verts, dl = [], []
    cmd = lambda a, b: dl.append(struct.pack('>II', a & 0xFFFFFFFF, b & 0xFFFFFFFF))
    for a, b in head_cmds: cmd(a, b)
    for m in used:
        tris = [f for f in F if f[0] == m]
        if m in tex:
            tid, w, h = tex[m]; line = (w + 7) // 8
            for a, b in ((0xE7000000, 0), (0xFCFF9880, 0xF514FEFF), (0xD7000002, 0xFFFFFFFF),
                         (0xF5080000 | (line << 9), (log2(h) << 14) | (log2(w) << 4)), (0xF2000000, (((w - 1) << 2) << 12) | ((h - 1) << 2)),
                         (0xFD100000, tid), (0xF5100000, 0x07000000), (0xE6000000, 0),
                         (0xF3000000, 0x07000000 | (min(2047, (w * h + 1) // 2 - 1) << 12)), (0xE7000000, 0),
                         (0xF5000100, 0x06000000), (0xE6000000, 0), (0xFD100000, 0x00400000 | tid), (0xF0000000, 0x063FC000),
                         (0xE7000000, 0), (0xDE000000, 0x08000040)):
                cmd(a, b)
            for a, b in per_tex_tail: cmd(a, b)
        else:
            w = h = 32
        batch, keys = [], {}
        def flush():
            if not batch: return
            base = len(verts)
            for k in sorted(keys, key=keys.get): verts.append(k)
            n = len(keys)
            cmd(0x01000000 | (n << 12) | ((n * 2) & 0xFF), 0x01000000 | (base * 16))
            for i in range(0, len(batch), 2):
                a = batch[i]
                if i + 1 < len(batch):
                    b = batch[i + 1]
                    cmd(0x06000000 | (a[0] * 2 << 16) | (a[1] * 2 << 8) | a[2] * 2, (b[0] * 2 << 16) | (b[1] * 2 << 8) | b[2] * 2)
                else:
                    cmd(0x05000000 | (a[0] * 2 << 16) | (a[1] * 2 << 8) | a[2] * 2, 0)
            batch.clear(); keys.clear()
        for f in tris:
            ks = []
            for vi, ti in f[1:]:
                u = VT[ti] if ti >= 0 else [0.0, 0.0]
                x, y, z = [int(round(c)) for c in P[vi]]
                s2 = max(-32768, min(32767, int(round(u[0] * w * 32)))); t2 = max(-32768, min(32767, int(round(-u[1] * h * 32))))
                ks.append(struct.pack('>hhhHhhBBBB', x, y, z, 0, s2, t2, 0xFE, 0xFE, 0xFE, 0xFF))
            if len(set(keys) | set(ks)) > 30: flush()
            t = []
            for k in ks:
                if k not in keys: keys[k] = len(keys)
                t.append(keys[k])
            batch.append(t)
        flush()
    cmd(0xDF000000, 0)
    return b''.join(verts), b''.join(dl), len(verts)
# ---- the pickup (bank 3's layout: +0 its list's offset, +4 its size, ..., +0x24 0x80000000, the vertices from +0x28 (segment 1 there))
vb, dlb, nv = build(place(-52.0, 119.0), [(0xD7000002, 0xFFFFFFFF)], [(0xD9F8FDFF, 0), (0xD9FFFFFF, 0x400)])
head = bytearray(pickup0[:0x28]); dl_off = 0x28 + len(vb)
head[0:8] = struct.pack('>II', dl_off, len(dlb))
pick = bytes(head) + vb + dlb
open(os.path.join(out, 'pickup.bin'), 'wb').write(pick)
# ---- held (bank 9's: +0 its lists' table, +4 its size, +0x10 the normals' table, +0x14 0x80000000 | its size, the vertices from +0x18
# (segment 1: the game copies them, from +0x18 up to the table, into two buffers of the weapon's own, and draws from those), the table
# right after them as in the original (0.11.4: ours had the display list between, and the game copied that too), the display list,
# then the normals: x and y per vertex slot, a 64-byte block for each batch (DC38000E before its vertices, as the original has), z in
# each vertex's flag. Batches share their vertices as obj_character.py's do (fewer records: the game keeps two copies of them).
def build_held(P):
    # (normals: from the faces, turned with the axes place() chose (a mirror flips the cross product: its determinant puts it back),
    # shared by vertices at the same place)
    M = np.zeros((3, 3)); M[0, thin] = 1; M[1, wide] = 1; M[2, ax] = 1
    sign = float(np.sign(np.linalg.det(M))) or 1.0
    N = np.zeros_like(P)
    for f in F:
        a, b, c = P[f[1][0]], P[f[2][0]], P[f[3][0]]
        n = np.cross(b - a, c - a) * sign
        for vi, ti in f[1:]:
            N[vi] += n
    N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-9)
    nb = lambda n: [int(np.clip(round(float(c) * 127.0), -127, 127)) & 0xFF for c in n]
    verts, dl, blocks, dc_at = [], [], [], []
    cmd = lambda a, b: dl.append(struct.pack('>II', a & 0xFFFFFFFF, b & 0xFFFFFFFF))
    cmd(0xE7000000, 0)
    for m in used:
        tris = [f for f in F if f[0] == m]
        if m in tex:
            tid, w, h = tex[m]; line = (w + 7) // 8
            for a, b in ((0xE7000000, 0), (0xFCFF9880, 0xF514FEFF), (0xD7000002, 0xFFFFFFFF),
                         (0xF5080000 | (line << 9), (log2(h) << 14) | (log2(w) << 4)), (0xF2000000, (((w - 1) << 2) << 12) | ((h - 1) << 2)),
                         (0xFD100000, tid), (0xF5100000, 0x07000000), (0xE6000000, 0),
                         (0xF3000000, 0x07000000 | (min(2047, (w * h + 1) // 2 - 1) << 12)), (0xE7000000, 0),
                         (0xF5000100, 0x06000000), (0xE6000000, 0), (0xFD100000, 0x00400000 | tid), (0xF0000000, 0x063FC000),
                         (0xE7000000, 0), (0xDE000000, 0x08000040), (0xDA380003, 0x03000000)):
                cmd(a, b)
        else:
            w = h = 32
        left = [tuple(f[1:]) for f in tris]
        users = {}
        for i, t in enumerate(left):
            for k in t:
                users.setdefault(k, set()).add(i)
        done = [False] * len(left)
        for start in range(len(left)):
            if done[start]:
                continue
            keys = set(left[start]); batch = [left[start]]; done[start] = True
            while True:
                cand = {}
                for k in keys:
                    for i in users[k]:
                        if not done[i]:
                            cand[i] = cand.get(i, 0) + 1
                best = None
                for i, shared in sorted(cand.items(), key=lambda kv: -kv[1]):
                    if len(keys | set(left[i])) <= 30:
                        best = i
                        break
                if best is None:
                    best = next((i for i in range(start + 1, len(left)) if not done[i] and len(keys | set(left[i])) <= 30), None)
                    if best is None:
                        break
                keys |= set(left[best]); batch.append(left[best]); done[best] = True
            order = sorted(keys)
            slot = {k: i for i, k in enumerate(order)}
            block = bytearray(64)
            base = len(verts)
            for k in order:
                vi, ti = k
                u = VT[ti] if ti >= 0 else [0.0, 0.0]
                x, y, z = [int(round(c)) for c in P[vi]]
                s2 = max(-32768, min(32767, int(round(u[0] * w * 32)))); t2 = max(-32768, min(32767, int(round(-u[1] * h * 32))))
                nx, ny, nz = nb(N[vi])
                block[2 * slot[k]] = nx; block[2 * slot[k] + 1] = ny
                verts.append(struct.pack('>hhhHhhBBBB', x, y, z, nz, s2, t2, 0xFF, 0xFF, 0xFF, 0xFF))
            dc_at.append((len(dl), len(blocks))); cmd(0xDC38000E, 0); blocks.append(block)
            n = len(order)
            cmd(0x01000000 | (n << 12) | ((n * 2) & 0xFF), 0x01000000 | (base * 16))
            tl = [[slot[k] for k in t] for t in batch]
            for i in range(0, len(tl), 2):
                a = tl[i]
                if i + 1 < len(tl):
                    b = tl[i + 1]
                    cmd(0x06000000 | (a[0] * 2 << 16) | (a[1] * 2 << 8) | a[2] * 2, (b[0] * 2 << 16) | (b[1] * 2 << 8) | b[2] * 2)
                else:
                    cmd(0x05000000 | (a[0] * 2 << 16) | (a[1] * 2 << 8) | a[2] * 2, 0)
    cmd(0xDF000000, 0)
    return b''.join(verts), dl, blocks, dc_at
vb, dl, blocks, dc_at = build_held(place(-45.0, 238.0))
tab_at = 0x18 + len(vb)
dl_at = tab_at + 8
lights_at = (dl_at + 8 * len(dl) + 7) & ~7
for i, k in dc_at:
    dl[i] = struct.pack('>II', 0xDC38000E, lights_at + 64 * k)
lights = b''.join(bytes(b) for b in blocks)
head = bytearray(held0[:0x18]); head[0:8] = struct.pack('>II', tab_at, 4)
head[0x10:0x18] = struct.pack('>II', lights_at, 0x80000000 | len(lights))
body = bytes(head) + vb + struct.pack('>II', dl_at, 0) + b''.join(dl)
body += bytes(lights_at - len(body)) + lights
open(os.path.join(out, 'held.bin'), 'wb').write(body)
print('pickup', len(pick), 'bytes (theirs %d), held' % len(pickup0), len(body), 'bytes (theirs %d;' % len(held0), len(vb) // 16, 'vertices,',
      len(blocks), 'batches),', len(tex), 'textures', 'axes', ax, wide, thin)

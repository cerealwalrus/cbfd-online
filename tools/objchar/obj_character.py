# A character exported as OBJ + MTL + PNG textures (a mesh with no skeleton: Jamjars, Banjo...) made into a model of the game's
# (bank 1's format) bound to model 3B's skeleton (Conker's 28 bones, the order and parents his animations need), to be drawn by an
# added character (host/src/mp_characters.cpp's extras[]). On this PC, with its own ROM (3B's skeleton is taken from it).
#   python3 -I tools/objchar/obj_character.py <folder with the .obj> <name> [--height=190] [--tex=0x2140] [--arms=down|keep] [--hints=...]
# Writes host/assets/<name>/<name>.bin (the model, unpacked) and host/assets/<name>/tex_XXXX.bin (its textures, CI8 + palette).
#
# The model: a 13-word header (2: the display lists' table, 3 its size; 4: the skeleton, 5 its size; 6: the textures' list (each: id,
# id, width, height), 7 its size; 8: the normals' table, 9 its size; 12: the size), 0x80000002, the vertex records from 0x38 (segment 1:
# x, y, z, the normal's z, s, t and the colour; each relative to its bone's joint), the display list (each batch's normals (DC38000E:
# 64 bytes, the normal's x and y for each of its 32 vertex slots), each bone's G_MTX from segment 3 at bone x 0x40, its vertices
# loaded, the triangles), the skeleton (3B's, its offsets ours), the textures' list and the normals' table.
# (0.11.4: Conker's microcode (RT64's F3DEX2CBFD) lights a character's vertex from its normal's x and y in the table its DC38000E
# names, at the vertex's slot, and its z in the vertex's flag, and multiplies its colour by the light. These models had 3B's own table
# (his vertices' normals, none of theirs), 0 for z and the normal's bytes as the colour: lit at random and tinted by their normals,
# dark and blotchy in a match.)
# Binding: the mesh stood on 3B's floor at the height asked, its arms (if it holds them out) turned down about the shoulders as 3B's
# hang, each vertex on the 3B bone whose segment (its joint to its child's) is nearest.
import sys, os, struct, glob
import numpy as np
from PIL import Image
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, '..', 'berri'))
from models import rom, entry, skeleton, world

src, name = sys.argv[1], sys.argv[2]
opt = lambda k, d: next((a.split('=', 1)[1] for a in sys.argv if a.startswith('--' + k + '=')), d)
HEIGHT = float(opt('height', '190'))
TEX_BASE = int(opt('tex', '0x2140'), 0)
ARMS = opt('arms', 'down')
out = os.path.join(here, '..', '..', 'host', 'assets', name)
os.makedirs(out, exist_ok=True)

r = rom()
T, _ = entry(r, 0x3B)
HT = struct.unpack('>13I', T[:52])
tb = skeleton(T)
JT = np.array(world(tb))
PAR = [b[0] for b in tb]

# ---- the mesh
objf = glob.glob(os.path.join(src, '*.obj'))[0]
mtlf = glob.glob(os.path.join(src, '*.mtl'))
mtl = {}
if mtlf:
    cur = None
    for line in open(mtlf[0], errors='ignore'):
        w = line.split()
        if not w:
            continue
        if w[0] == 'newmtl':
            cur = w[1]; mtl[cur] = None
        elif w[0] == 'map_Kd' and cur:
            mtl[cur] = os.path.join(src, os.path.basename(' '.join(w[1:]).replace('\\', '/')))
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
V = np.array(V, float)
# (up is y; facing +z as 3B faces: the mesh's longest flat side taken as its front is not guessed: --turn=degrees about y if it faces away)
turn = np.radians(float(opt('turn', '0')))
c, s = np.cos(turn), np.sin(turn)
V = V @ np.array([[c, 0, -s], [0, 1, 0], [s, 0, c]]).T
# (a mesh facing away is turned with --turn=180: its feet aren't a safe guide (a boot's heel can reach back past its toe);
# four legs are turned by tools/objchar/autorig.py, the bulkier end taken as the head)
lo, hi = V.min(0), V.max(0)
scale = HEIGHT / max(1e-6, hi[1] - lo[1])
floor = min(JT[19][1], JT[23][1])
V = (V - [(lo[0] + hi[0]) / 2, lo[1], (lo[2] + hi[2]) / 2]) * scale + [0, floor, 0]
print('vertices', len(V), 'triangles', len(faces), 'height', round(V[:, 1].max() - V[:, 1].min()))

# ---- its own skeleton fitted and each vertex bound: tools/objchar/autorig.py (its limbs found along its surface, its arms turned to
# hang as 3B's in his bind pose)
sys.path.insert(0, here)
import autorig
tri_idx = np.array([[f[1][0], f[2][0], f[3][0]] for f in faces])
quad = {'yes': True, 'no': False}.get(opt('quadruped', ''), None)
# (--hints=key:value,...: what the mesh's shape can't settle, said for it (autorig.rig's hints): legs:none, legs:rigid, hip:0.3,
# head:spine, feet:stiff, arm+1:none, arm-1:none)
hints = dict(kv.split(':', 1) for kv in opt('hints', '').split(',') if ':' in kv)
V, J, bone = autorig.rig(V, tri_idx, JT, floor, tail=opt('tail', 'drop') == 'keep', log=print, quadruped=quad, hints=hints)
counts = np.bincount(bone, minlength=len(PAR))
print('vertices per bone', counts.tolist())
OWN = True
FIT = ''

# ---- normals (per vertex, from the faces; vertices at the same place (a mesh split at its materials' edges) share theirs, so the
# light has no seams where the mesh has none)
N = np.zeros_like(V)
for f in faces:
    a, b, c3 = V[f[1][0]], V[f[2][0]], V[f[3][0]]
    n = np.cross(b - a, c3 - a)
    for p in f[1:]:
        N[p[0]] += n
_pos = {}
for i, p in enumerate(np.round(V, 3)):
    _pos.setdefault(tuple(p), []).append(i)
for same in _pos.values():
    if len(same) > 1:
        N[same] = N[same].sum(0)
N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-9)
def normal_bytes(n):
    return [int(np.clip(round(float(c) * 127.0), -127, 127)) & 0xFF for c in n]

# ---- textures
def pow2(n):
    p = 1
    while p < n:
        p *= 2
    return p
def to_ci8(img):
    img = img.convert('RGBA')
    w, h = max(8, min(64, pow2(img.size[0]))), max(8, min(64, pow2(img.size[1])))   # (8 at least: a row's 64 bits)
    while w * h > 2048:
        if w >= h: w //= 2
        else: h //= 2
    img = img.resize((w, h), Image.LANCZOS)
    a = np.asarray(img)
    q = img.convert('RGB').quantize(colors=255, method=Image.MEDIANCUT, dither=Image.NONE)
    idx = np.asarray(q).astype(np.uint8) + 1
    idx[a[:, :, 3] < 128] = 0
    pal = (q.getpalette() or [])[:255 * 3]; pal += [0] * (255 * 3 - len(pal))
    c4 = lambda r_, g, b_: ((r_ >> 3) << 11) | ((g >> 3) << 6) | ((b_ >> 3) << 1) | 1
    pal16 = [0] + [c4(pal[3 * i], pal[3 * i + 1], pal[3 * i + 2]) for i in range(255)]
    px = idx.copy()
    for y in range(1, h, 2):
        px[y] = px[y].reshape(-1, 2, 4)[:, ::-1, :].reshape(-1)
    return w, h, px.tobytes() + b''.join(struct.pack('>H', v) for v in pal16)
used = []
for f in faces:
    if f[0] not in used:
        used.append(f[0])
tex = {}
for m in used:
    path = mtl.get(m)
    if not path or not os.path.exists(path):
        continue
    tid = TEX_BASE + len(tex)
    w, h, data = to_ci8(Image.open(path))
    tex[m] = (tid, w, h)
    open(os.path.join(out, 'tex_%04X.bin' % tid), 'wb').write(data)
print('textures', len(tex), 'ids %04X to %04X' % (TEX_BASE, TEX_BASE + len(tex) - 1))

# ---- the model
records = []      # vertex records
dl = []
def cmd(w0, w1):
    dl.append(struct.pack('>II', w0 & 0xFFFFFFFF, w1 & 0xFFFFFFFF))
def log2(n):
    return n.bit_length() - 1
light_cmds = []      # (each batch's DC38000E: its place in the list, its block of the normals' table)
normal_blocks = []
cmd(0xE7000000, 0)
cmd(0xD9FFFFFF, 0x00000400)
for m in used:
    tris = [f for f in faces if f[0] == m]
    if m in tex:
        tid, w, h = tex[m]
        # (CI8 with its palette, loaded as 3B's own are, and the game's own state for a character set by its call to segment 8's
        # display list (DE 08000040), as 3B's does: setting the render mode here ourselves (TLUT, cycle) left it changed for whatever
        # was drawn after, and the screen went black)
        line = (w + 7) // 8
        cmd(0xE7000000, 0)
        cmd(0xFCFF9880, 0xF514FEFF)          # (3B's colour combiner: the texture lit)
        cmd(0xD7000002, 0xFFFFFFFF)
        cmd(0xF5080000 | (line << 9), (log2(h) << 14) | (log2(w) << 4))   # tile 0: 8-bit, its palette's (TLUT on, by segment 8's list)
        cmd(0xF2000000, (((w - 1) << 2) << 12) | ((h - 1) << 2))
        cmd(0xFD100000, tid)
        cmd(0xF5100000, 0x07000000)
        cmd(0xE6000000, 0)
        cmd(0xF3000000, 0x07000000 | (min(2047, (w * h + 1) // 2 - 1) << 12))
        cmd(0xE7000000, 0)
        cmd(0xF5000100, 0x06000000)
        cmd(0xE6000000, 0)
        cmd(0xFD100000, 0x00400000 | tid)
        cmd(0xF0000000, 0x063FC000)
        cmd(0xE7000000, 0)
        cmd(0xDE000000, 0x08000040)
    else:
        w = h = 32
    # batches: up to 30 vertex slots; within a batch, each bone's vertices loaded under its matrix
    pending = []
    def flush():
        if not pending:
            return
        slots = {}
        by_bone = {}
        for t in pending:
            for (vi, ti) in t:
                key = (vi, ti)
                if key not in slots:
                    slots[key] = None
                    by_bone.setdefault(int(bone[vi]), []).append(key)
        # (the batch's normals: x and y for each slot, in the table; DC38000E before its vertices are loaded, as 3B's has one before
        # each of his batches: the vertices are lit as they're loaded)
        block = bytearray(64)
        light_cmds.append((len(dl), len(normal_blocks))); cmd(0xDC38000E, 0)
        normal_blocks.append(block)
        at = 0
        for b in sorted(by_bone):
            keys = by_bone[b]
            first = len(records)
            for (vi, ti) in keys:
                x, y, z = np.round(V[vi] - J[b]).astype(int)
                u = VT[ti] if ti >= 0 else [0.0, 0.0]
                s2 = int(round(u[0] * w * 32)); t2 = int(round(-u[1] * h * 32))
                nx, ny, nz = normal_bytes(N[vi])
                block[2 * at] = nx; block[2 * at + 1] = ny
                # (its colour: white, as most of 3B's are, a little darker facing down (under the arms, the chin), as his darker
                # ones are; the light does the rest)
                g = int(round(255 * (0.82 + 0.18 * min(1.0, max(0.0, 0.5 + 0.5 * float(N[vi][1]) * 1.6)))))
                records.append(struct.pack('>hhhHhhBBBB', int(x), int(y), int(z), nz, max(-32768, min(32767, s2)),
                                           max(-32768, min(32767, t2)), g, g, g, 255))
                slots[(vi, ti)] = at
                at += 1
            cmd(0xDA380003, 0x03000000 | (b * 0x40))
            nn = len(keys); v0 = at - nn
            cmd(0x01000000 | (nn << 12) | (((v0 + nn) * 2) & 0xFF), 0x01000000 | (0x38 + first * 16))   # (segment 1 is the model's start: its vertex records from 0x38, as 3B's 0101F03E 01000038)
        for i in range(0, len(pending), 2):
            a = [slots[k2] for k2 in pending[i]]
            if i + 1 < len(pending):
                b2 = [slots[k2] for k2 in pending[i + 1]]
                cmd(0x06000000 | (a[0] * 2 << 16) | (a[1] * 2 << 8) | a[2] * 2, (b2[0] * 2 << 16) | (b2[1] * 2 << 8) | b2[2] * 2)
            else:
                cmd(0x05000000 | (a[0] * 2 << 16) | (a[1] * 2 << 8) | a[2] * 2, 0)
        pending.clear()
    def nslots(tris_):
        return len({k2 for t in tris_ for k2 in t})
    # (greedy: each batch filled with the triangles sharing the most of its vertices, so fewer vertex records: the model must fit
    # the room of the entry it's drawn from)
    left = [[(p[0], p[1]) for p in f[1:]] for f in tris]
    users = {}
    for i, t in enumerate(left):
        for k2 in t:
            users.setdefault(k2, set()).add(i)
    done = [False] * len(left)
    for start in range(len(left)):
        if done[start]:
            continue
        keys = set(left[start]); pending.append(left[start]); done[start] = True
        while True:
            cand = {}
            for k2 in keys:
                for i in users[k2]:
                    if not done[i]:
                        cand[i] = cand.get(i, 0) + 1
            best = None
            for i, shared in sorted(cand.items(), key=lambda kv: -kv[1]):
                if len(keys | set(left[i])) <= 30:
                    best = i
                    break
            if best is None:
                # (none sharing fits: the next unshared one in order, if it fits)
                nxt = next((i for i in range(start + 1, len(left)) if not done[i] and len(keys | set(left[i])) <= 30), None)
                if nxt is None:
                    break
                best = nxt
            keys |= set(left[best]); pending.append(left[best]); done[best] = True
        flush()
    flush()
cmd(0xDF000000, 0)

vbytes = b''.join(records)
dl_at = 0x38 + len(vbytes)
dl_bytes_len = len(dl) * 8
tab_at = dl_at + dl_bytes_len
skel_at = tab_at + 4
skel = bytearray(T[HT[4]:HT[4] + 28 * 16])
if OWN and FIT != 'shoulders':
    for i in range(28):
        p_ = PAR[i]
        off = J[i] - (J[p_] if p_ != 255 else np.zeros(3))
        skel[16 * i + 4:16 * i + 16] = struct.pack('>3f', *[float(x) for x in off])
skel = bytes(skel)
texl = b''.join(struct.pack('>IIHH', tid, tid, w, h) for (tid, w, h) in tex.values())
texl_at = skel_at + len(skel)
lights_at = texl_at + len(texl)
lights_at = (lights_at + 7) & ~7
lights = b''.join(bytes(blk) for blk in normal_blocks)
for i, k in light_cmds:
    dl[i] = struct.pack('>II', 0xDC38000E, lights_at + 64 * k)
total = lights_at + len(lights)
head = struct.pack('>13I', 0, 0, tab_at, 4, skel_at, len(skel), texl_at, len(texl), lights_at, len(lights), 0, 0, total)
body = head + struct.pack('>I', 0x80000002) + vbytes + b''.join(dl) + struct.pack('>I', dl_at) + skel + texl
body += bytes(lights_at - len(body)) + lights
assert len(body) == total
open(os.path.join(out, name + '.bin'), 'wb').write(body)
print('written', total, 'bytes:', len(records), 'vertex records,', len(dl), 'commands')

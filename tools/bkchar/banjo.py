# Banjo: Banjo-Tooie's own model of him (its file 0x62B, tools/bkchar/src/banjo_0x62B.bin, the block as its ROM has it) made into a
# model of the game's, to be moved by Banjo-Kazooie's own animations (host/src/bk_banjo.cpp plays them as Banjo-Kazooie does), taken
# from that game's ROM on this PC (not in the project):
#   python3 -I tools/bkchar/banjo.py tools/bkchar/src/banjo_0x62B.bin <Banjo-Kazooie (USA) ROM>
# Writes host/assets/banjo/banjo.bin (the model), tex_2160.bin on (its textures) and bk_anims.bin (Banjo-Kazooie's skeleton of him and
# his animations, read as the game runs). Neither game's ROM is put in the project.
#
# The model: as tools/objchar/obj_character.py's (a 13-word header, the vertex records from 0x38 relative to their bone's joint, the
# display list, the skeleton, the textures' list, the normals' table), Banjo-Tooie's triangles as its display lists draw them (each
# vertex on the bone whose matrix was loaded when it was: tools/bkchar/btmodel.py), each of his bones on one of the game's 28 (BONE_SLOT
# below), Kazooie hidden in the backpack as Banjo-Kazooie hides her, his eyes three ways (open, half, shut) on bones of their own
# (shown one at a time: he blinks).
import sys, os, struct, zlib
import numpy as np
from PIL import Image
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
from btmodel import BT
import bkanim

bt_path, bk_rom = sys.argv[1], sys.argv[2]
out = next((a.split('=', 1)[1] for a in sys.argv if a.startswith('--out=')), os.path.join(here, '..', '..', 'host', 'assets', 'banjo'))
os.makedirs(out, exist_ok=True)
TEX_BASE = 0x2160
WHITE = '--white' in sys.argv     # (test: the untextured parts drawn with a white texture, as the other characters draw)
F = 1.5            # Banjo-Kazooie's units to the model's (he stands 121 tall in them: 181 here, as Conker's 182)

# ---- Banjo-Tooie's file: a ROM block (its size / 16, then deflate) or already unpacked (0x0000000B first)
raw = open(bt_path, 'rb').read()
if raw[:4] != b'\x00\x00\x00\x0b':
    raw = zlib.decompressobj(-15).decompress(raw[2:])
bt_file = os.path.join(out, '.bt_unpacked.tmp')
open(bt_file, 'wb').write(raw)

# ---- Banjo-Kazooie's skeleton of him (model 0x34E) and its animations
sys.path.insert(0, os.path.join(here))
import bkrom
BK = bkrom.Assets(bk_rom)
bk_model = BK.get(0x34E)
K, SK = bkanim.load_skeleton(bk_model)

# ---- the appendages shown (Banjo-Kazooie's player model's: the backpack, Kazooie's upper half, legs and tail hidden; the necklace)
SEL = {7: 1, 18: 1, 15: 0, 16: 0, 17: 0, 0x38: 0, 0x39: 0}
EYES = {14: 1, 15: 5, 19: 8}       # the game's bone for each look of the eyes: Banjo-Tooie's choice (1 open, 5 half, 8 shut)
bt = BT(bt_file, selectors=SEL)
eye_sets = {}
for slot, choice in EYES.items():
    s = dict(SEL); s[0x38] = choice; s[0x39] = choice
    e = BT(bt_file, selectors=s)
    base = {}
    for t in bt.tris:
        key = (t[0], tuple(tuple(c) for c in t[1]))
        base[key] = base.get(key, 0) + 1
    extra = []
    for t in e.tris:
        key = (t[0], tuple(tuple(c) for c in t[1]))
        if base.get(key, 0) > 0:
            base[key] -= 1
        else:
            extra.append(t)
    eye_sets[slot] = (e, extra)
os.remove(bt_file)

# ---- Banjo-Tooie's bones to Banjo-Kazooie's (by id, down any chain of bones at the same pivot that Banjo-Tooie folded into one) and
# to the game's 28
ids_bt = {b['id'] for b in bt.bones}
by_id = {bid: i for i, (p, bid, par) in enumerate(SK)}
kids = {}
for i, (p, bid, par) in enumerate(SK):
    kids.setdefault(par, []).append(i)
BT2BK = []
for b in bt.bones:
    k = by_id.get(b['id'])
    if k is not None:
        while True:
            ch = [c for c in kids.get(k, []) if np.allclose(SK[c][0], SK[k][0]) and SK[c][1] not in ids_bt]
            if len(ch) != 1:
                break
            k = ch[0]
    BT2BK.append(k)
for i, b in enumerate(bt.bones):           # (Banjo-Tooie's own bones: with their parent)
    if BT2BK[i] is None:
        BT2BK[i] = BT2BK[b['par']]
# the game's 28 (3B's: 0 root, 1 chest, 2-4 left arm, 7-9 right arm (9 the hand holding the weapon), 12 head, 16-18 left leg, 20-22
# right leg): Banjo-Tooie's bone -> slot
BONE_SLOT = {0: 0, 1: 24, 2: 20, 3: 21, 4: 22, 5: 16, 6: 17, 7: 18, 8: 1, 9: 12, 10: 12, 11: 13, 12: 2, 13: 3, 14: 4, 15: 7, 16: 8,
             17: 9, 18: 1, 19: 1, 20: 5, 21: 6, 22: 10, 23: 10, 24: 11}
SLOT_BK = [-1] * 28
for btb, slot in BONE_SLOT.items():
    SLOT_BK[slot] = BT2BK[btb]
for slot in EYES:
    SLOT_BK[slot] = BT2BK[9]
# (the rest: their neighbour's, so anything drawn on them goes with him)
for slot, like in ((23, 22), (25, 24), (26, 24), (27, 24)):
    SLOT_BK[slot] = SLOT_BK[like]
assert all(k >= 0 for k in SLOT_BK)
print('slots -> Banjo-Kazooie bones', SLOT_BK)

# the arms' bind turns: his arms are held straight out to the sides (as Banjo-Kazooie's skeleton has them); the game's weapon is held
# in a hand hanging down as Conker's does (3B's joints, below). Each arm bone's vertices kept turned to hang as Conker's, so the hand
# holds the game's weapons as his does and the arms can be posed by the game's aim; Banjo-Kazooie's turns undo it (B^-1).
J3B = {2: (22.9, 99.5, -3.2), 3: (37.8, 79.5, -4.7), 4: (46.3, 63.1, -3.7), 5: (54.0, 48.8, -3.6),
       7: (-23.7, 98.3, -3.4), 8: (-35.5, 79.8, -4.8), 9: (-46.4, 63.0, -3.9), 10: (-53.5, 49.3, -3.9)}
def align(a, b):
    a = np.array(a, float); b = np.array(b, float)
    a /= np.linalg.norm(a); b /= np.linalg.norm(b)
    v = np.cross(a, b); s = np.linalg.norm(v); c = float(np.dot(a, b))
    if s < 1e-9:
        return np.eye(3)
    kx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + kx + kx @ kx * ((1 - c) / (s * s))
piv = lambda k: np.array(SK[k][0], float)
B = [np.eye(3) for _ in range(28)]
for (a, b_, c) in ((2, 3, 4), (7, 8, 9)):
    # upper arm: shoulder -> elbow; forearm: elbow -> wrist; hand: wrist -> its end (the hand's reference point, 62.5 out)
    seg = {a: (piv(SLOT_BK[a]), piv(SLOT_BK[b_])), b_: (piv(SLOT_BK[b_]), piv(SLOT_BK[c]))}
    side = 1 if a == 2 else -1
    seg[c] = (piv(SLOT_BK[c]), np.array([62.5 * side, 72.5, -1.5]))
    for s_ in (a, b_, c):
        src = seg[s_][1] - seg[s_][0]
        dst = np.array(J3B[s_ + 1]) - np.array(J3B[s_])
        B[s_] = align(src, dst)

# ---- the textures (CI8 + palette, as obj_character.py's, odd rows' words swapped for the load)
def tex_rgba(t):
    base = bt.tex_data + t['off']
    m = bt.m
    w, h = t['w'], t['h']
    img = Image.new('RGBA', (w, h))
    if t['type'] == 1:
        pal = [struct.unpack('>H', m[base + 2 * k:base + 2 * k + 2])[0] for k in range(16)]
        px = m[base + 0x20:base + 0x20 + w * h // 2]
        get = lambda x, y: (px[(y * w + x) // 2] >> (4 if x % 2 == 0 else 0)) & 15
    elif t['type'] == 2:
        pal = [struct.unpack('>H', m[base + 2 * k:base + 2 * k + 2])[0] for k in range(256)]
        px = m[base + 0x200:base + 0x200 + w * h]
        get = lambda x, y: px[y * w + x]
    else:
        raise SystemExit('texture type %d not handled' % t['type'])
    for y in range(h):
        for x in range(w):
            c = pal[get(x, y)]
            img.putpixel((x, y), (((c >> 11) & 31) * 255 // 31, ((c >> 6) & 31) * 255 // 31, ((c >> 1) & 31) * 255 // 31, 255 if c & 1 else 0))
    return img
def to_ci8(img):
    w, h = img.size
    a = np.asarray(img)
    cols = {}
    idx = np.zeros((h, w), np.uint8)
    for y in range(h):
        for x in range(w):
            r, g, b_, al = [int(c) for c in a[y, x]]
            if al < 128:
                idx[y, x] = 0
                continue
            key = (r >> 3, g >> 3, b_ >> 3)
            if key not in cols:
                cols[key] = len(cols) + 1
            idx[y, x] = cols[key]
    assert len(cols) <= 255
    pal16 = [0] * 256
    for (r, g, b_), i in cols.items():
        pal16[i] = (r << 11) | (g << 6) | (b_ << 1) | 1
    px = idx.copy()
    for y in range(1, h, 2):
        px[y] = px[y].reshape(-1, 2, 4)[:, ::-1, :].reshape(-1)
    return px.tobytes() + b''.join(struct.pack('>H', v) for v in pal16), bool((idx == 0).any())

# ---- the triangles: (texture, tile settings, [(vertex, slot)] x 3), from the model and the eyes
tris = []
MODELS = [bt]
def take(model, tlist, slot_of):
    if model not in MODELS:
        MODELS.append(model)
    for tex, corners, comb in tlist:
        tris.append((model, tex, [(vi, slot_of(b)) for vi, b in corners]))
take(bt, bt.tris, lambda b: BONE_SLOT[b])
for slot, (e, extra) in eye_sets.items():
    take(e, extra, lambda b, slot=slot: slot)
mid = lambda model: MODELS.index(model)       # (a model's number, for keys that come out the same each time)
print('triangles', len(tris))

# texture ids
used_tex = []
for model, tex, c in tris:
    if tex >= 0 and tex not in used_tex:
        used_tex.append(tex)
assert len(used_tex) <= 32, used_tex
tex_info = {}
for n, ti in enumerate(used_tex):
    t = bt.textures[ti]
    data, cut = to_ci8(tex_rgba(t))
    tid = TEX_BASE + n
    open(os.path.join(out, 'tex_%04X.bin' % tid), 'wb').write(data)
    tex_info[ti] = (tid, t['w'], t['h'], cut)
if WHITE:
    tid = TEX_BASE + len(used_tex)
    open(os.path.join(out, 'tex_%04X.bin' % tid), 'wb').write(bytes([1] * 64) + struct.pack('>H', 0) + struct.pack('>H', 0xFFFF) + bytes(508))
    tex_info[-1] = (tid, 8, 8, False)
    used_tex.append(-1)
for old in range(TEX_BASE + len(used_tex), TEX_BASE + 0x20):        # (the old Banjo's left over)
    p = os.path.join(out, 'tex_%04X.bin' % old)
    if os.path.exists(p):
        os.remove(p)
print('textures', len(used_tex), ['%X' % tex_info[t][0] for t in used_tex])
# each texture's tile settings as Banjo-Tooie draws it (clamped or wrapped, its masks): from the F5 after its load
tile_of = {}
G = bt.G
for i, (w0, w1) in enumerate(G):
    if w0 >> 24 == 0xFD and (w1 >> 24) == 2 and ((w0 >> 21) & 7) != 0:
        off = w1 & 0xFFFFFF
        for ti, t in enumerate(bt.textures):
            if off == t['off'] + (0x20 if t['type'] == 1 else 0x200 if t['type'] == 2 else 0):
                for j in range(i, min(i + 8, len(G))):
                    if G[j][0] >> 24 == 0xF5 and ((G[j][1] >> 24) & 7) == 0:
                        tile_of[ti] = G[j][1] & 0x000FFFFF
                        break

# ---- vertices: each (vertex, slot) once in a batch, relative to the slot's joint (Banjo-Kazooie's pivot of its bone, times F), turned
# by the slot's bind turn; s and t halved (Banjo-Tooie draws at G_TEXTURE's 0.5, ours at 1)
def local(model, vi, slot):
    v = model.V[vi]
    p = np.array(v[:3], float)
    q = B[slot] @ (p - piv(SLOT_BK[slot])) * F
    n = np.array(v[9:12], float)
    n = B[slot] @ n
    nn = np.linalg.norm(n)
    n = n / nn if nn > 1e-6 else np.array([0, 1.0, 0])
    return q, n, v[3], v[4], v[5:9]
def nbyte(c):
    return int(np.clip(round(float(c) * 127.0), -127, 127)) & 0xFF

records, dl = [], []
def cmd(w0, w1):
    dl.append(struct.pack('>II', w0 & 0xFFFFFFFF, w1 & 0xFFFFFFFF))
light_cmds, normal_blocks = [], []
cmd(0xE7000000, 0)
cmd(0xD9FFFFFF, 0x00000400)
# materials: untextured (the vertex colours lit) first, then each texture
mats = ([-1] + used_tex) if not WHITE else used_tex
for mat in mats:
    mt = [t for t in tris if t[1] == mat]
    if not mt:
        continue
    if mat < 0 and not WHITE:
        cmd(0xE7000000, 0)
        cmd(0xFCFFFE80, 0xF51778FF)          # (cycle 1: 1; cycle 2 as Conker's: (shade - env) x it + prim)
        cmd(0xD7000000, 0xFFFFFFFF)
        cmd(0xDE000000, 0x08000040)
        w = h = 32
        cut = False
    else:
        tid, w, h, cut = tex_info[mat]
        line = (w + 7) // 8
        log2 = lambda n: n.bit_length() - 1
        tl = tile_of.get(mat, (log2(h) << 14) | (log2(w) << 4))
        cmd(0xE7000000, 0)
        cmd(0xFCFF9880, 0xF514FEFF)
        cmd(0xD7000002, 0xFFFFFFFF)
        cmd(0xF5080000 | (line << 9), tl)
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
        if cut:
            cmd(0xE2001300, 0x00001000)
    pending = []
    def flush():
        if not pending:
            return
        slots, by_slot = {}, {}
        for model, tex, corners in pending:
            for vi, slot in corners:
                key = (mid(model), vi, slot)
                if key not in slots:
                    slots[key] = None
                    by_slot.setdefault(slot, []).append((model, vi, slot))
        block = bytearray(64)
        light_cmds.append((len(dl), len(normal_blocks)))
        cmd(0xDC38000E, 0)
        normal_blocks.append(block)
        at = 0
        for slot in sorted(by_slot):
            keys = by_slot[slot]
            first = len(records)
            for model, vi, s_ in keys:
                q, n, s0, t0, col = local(model, vi, s_)
                x, y, z = [int(round(c)) for c in q]
                block[2 * at] = nbyte(n[0]); block[2 * at + 1] = nbyte(n[1])
                records.append(struct.pack('>hhhHhhBBBB', x, y, z, nbyte(n[2]), int(round(s0 * 0.5)), int(round(t0 * 0.5)),
                                           col[0], col[1], col[2], 255))
                slots[(mid(model), vi, s_)] = at
                at += 1
            cmd(0xDA380003, 0x03000000 | (slot * 0x40))
            nn = len(keys); v0 = at - nn
            cmd(0x01000000 | (nn << 12) | (((v0 + nn) * 2) & 0xFF), 0x01000000 | (0x38 + first * 16))
        for i in range(0, len(pending), 2):
            a = [slots[(mid(pending[i][0]), vi, s_)] for vi, s_ in pending[i][2]]
            if i + 1 < len(pending):
                b2 = [slots[(mid(pending[i + 1][0]), vi, s_)] for vi, s_ in pending[i + 1][2]]
                cmd(0x06000000 | (a[0] * 2 << 16) | (a[1] * 2 << 8) | a[2] * 2, (b2[0] * 2 << 16) | (b2[1] * 2 << 8) | b2[2] * 2)
            else:
                cmd(0x05000000 | (a[0] * 2 << 16) | (a[1] * 2 << 8) | a[2] * 2, 0)
        pending.clear()
    # batches of up to 30 vertex slots, each filled with the triangles sharing the most of its vertices
    keys_of = [set((mid(t[0]), vi, s_) for vi, s_ in t[2]) for t in mt]
    users = {}
    for i, ks in enumerate(keys_of):
        for k2 in ks:
            users.setdefault(k2, set()).add(i)
    done = [False] * len(mt)
    for start in range(len(mt)):
        if done[start]:
            continue
        keys = set(keys_of[start]); pending.append(mt[start]); done[start] = True
        while True:
            cand = {}
            for k2 in keys:
                for i in users[k2]:
                    if not done[i]:
                        cand[i] = cand.get(i, 0) + 1
            best = None
            for i, shared in sorted(cand.items(), key=lambda kv: -kv[1]):
                if len(keys | keys_of[i]) <= 30:
                    best = i
                    break
            if best is None:
                nxt = next((i for i in range(start + 1, len(mt)) if not done[i] and len(keys | keys_of[i]) <= 30), None)
                if nxt is None:
                    break
                best = nxt
            keys |= keys_of[best]; pending.append(mt[best]); done[best] = True
        flush()
    flush()
    if cut:
        cmd(0xE2001300, 0)
cmd(0xDF000000, 0)

# ---- the skeleton, for the game's own use of it (its own matrices for him, which bk_banjo.cpp's replace as he's drawn: its facing and
# its aim are all of them that's kept): a man's of Conker's bones (Cloud's, fitted on his own model; standing on Conker's floor), so the
# game poses it as it does the others'
PAR = [255, 0, 1, 2, 3, 4, 4, 1, 7, 8, 9, 9, 1, 12, 12, 12, 0, 16, 17, 18, 0, 20, 21, 22, 0, 24, 25, 26]
J = [(0.0, 31.1, 0.8), (0.0, 47.4, 0.8), (22.9, 99.5, -3.2), (37.8, 79.5, -4.7), (46.3, 63.1, -3.7), (54.0, 48.8, -3.6), (47.4, 55.7, 8.0),
     (-23.7, 98.3, -3.4), (-35.5, 79.8, -4.8), (-46.4, 63.0, -3.9), (-53.5, 49.3, -3.9), (-47.3, 55.8, 7.6), (0.0, 122.2, 0.8),
     (4.9, 138.5, 7.9), (-4.2, 138.6, 8.1), (0.0, 128.2, 10.8), (9.6, 21.6, 0.8), (9.6, -5.5, -3.0), (9.6, -32.6, -10.6),
     (9.6, -41.1, 18.1), (-8.5, 21.6, 0.8), (-8.4, -5.5, -3.0), (-8.4, -32.6, -10.6), (-8.4, -41.1, 18.1), (0.0, 31.1, -8.7),
     (0.0, 31.1, -18.2), (0.0, 31.1, -27.7), (0.0, 31.1, -37.2)]
J = [np.array(j) for j in J]
skel = bytearray()
for s in range(28):
    p_ = PAR[s]
    off = J[s] - (J[p_] if p_ != 255 else np.zeros(3))
    skel += struct.pack('>BBBx3f', p_, s, s, *[float(x) for x in off])     # (its parent, then its own number twice, as 3B's)
skel = bytes(skel)

vbytes = b''.join(records)
dl_at = 0x38 + len(vbytes)
tab_at = dl_at + len(dl) * 8
skel_at = tab_at + 4
texl = b''.join(struct.pack('>IIHH', tid, tid, w, h) for (tid, w, h, cut) in (tex_info[t] for t in used_tex))

texl_at = skel_at + len(skel)
lights_at = (texl_at + len(texl) + 7) & ~7
lights = b''.join(bytes(b_) for b_ in normal_blocks)
for i, k in light_cmds:
    dl[i] = struct.pack('>II', 0xDC38000E, lights_at + 64 * k)
total = lights_at + len(lights)
head = struct.pack('>13I', 0, 0, tab_at, 4, skel_at, len(skel), texl_at, len(texl), lights_at, len(lights), 0, 0, total)
body = head + struct.pack('>I', 0x80000002) + vbytes + b''.join(dl) + struct.pack('>I', dl_at) + skel + texl
body += bytes(lights_at - len(body)) + lights
assert len(body) == total
open(os.path.join(out, 'banjo.bin'), 'wb').write(body)
print('banjo.bin', total, 'bytes:', len(records), 'vertex records,', len(dl), 'commands')

# ---- bk_anims.bin: "BKAN", version 1; F; the anim list's scale K; the skeleton (count; each: pivot xyz, id, parent); the 28 slots'
# Banjo-Kazooie bones (-1 none) and bind turns (B by rows: a slot's vertices are B (p - pivot) F); the animations (count; each: id,
# offset, size), their bytes as Banjo-Kazooie has them
ANIMS = [0x01, 0x02, 0x03, 0x05, 0x08, 0x09, 0x0A, 0x0C, 0x0E, 0x0F, 0x2E, 0x39, 0x4D, 0x57, 0x59, 0x5A, 0x68, 0x6F, 0x70, 0x71, 0x72,
         0x73, 0x77, 0x95, 0xB0, 0xB1, 0xB9, 0xD2, 0xF6, 0x10C, 0x116, 0x11B, 0x149]
pack = bytearray(b'BKAN')
pack += struct.pack('>Iff', 1, F, K)
pack += struct.pack('>I', len(SK))
for p, bid, par in SK:
    pack += struct.pack('>3fhh', float(p[0]), float(p[1]), float(p[2]), bid, par)
for s in range(28):
    pack += struct.pack('>i', SLOT_BK[s])
    pack += struct.pack('>9f', *[float(x) for x in B[s].reshape(-1)])
blobs = [(a, BK.get(a)) for a in ANIMS]
pack += struct.pack('>I', len(blobs))
at = len(pack) + 12 * len(blobs)
for a, b_ in blobs:
    pack += struct.pack('>III', a, at, len(b_))
    at += (len(b_) + 3) & ~3
for a, b_ in blobs:
    pack += b_ + bytes(((len(b_) + 3) & ~3) - len(b_))
open(os.path.join(out, 'bk_anims.bin'), 'wb').write(pack)
print('bk_anims.bin', len(pack), 'bytes,', len(blobs), 'animations')

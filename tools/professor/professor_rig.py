# The Professor (bank 1 entry 0x99: 32 bones, in his chair: his claws, his eye on its stalk, the chair's joysticks) rebuilt on model 3B's skeleton (Conker's 28 bones, the order and parents
# Conker's animations need) at his own joints: his own proportions, Conker's moves. On this PC only, from its own ROM; the output is
# never committed (*.bin). His display list, textures and materials are kept; each vertex record is put in its new bone's space and the
# G_MTX loads pointed at the new bones. The same way as Berri's (tools/berri/berri_rig.py --own).
#   python3 -I tools/professor/professor_rig.py [height] [--drop=N]
#     height: his height in 3B's units (3B is 182 from his feet to his ears; Berri 205), default 250
#     --drop: his skeleton lowered by N (Berri's feet stood off the floor by about 12 with Conker's animations), default 12
import struct, zlib, sys, os, numpy as np
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, '..', 'berri'))
from models import rom, entry, skeleton, world
MODEL, BASE = 0x99, 0x3B
args = [a for a in sys.argv[1:] if not a.startswith('--')]
HEIGHT = float(args[0]) if args else 340.0   # (with his chair's aerial: his body about Conker's size)
DROP = next((float(a.split('=')[1]) for a in sys.argv if a.startswith('--drop=')), 0.0)   # (his chair's bottom his floor: on 3B's hips, no legs to bend)
r = rom()
B, _ = entry(r, MODEL); T, _ = entry(r, BASE)
B = bytearray(B)
HB = list(struct.unpack('>13I', B[:52]))
bb = skeleton(bytes(B)); JB = [np.array(w) for w in world(bb)]
tb = skeleton(T); JT = [np.array(w) for w in world(tb)]
dl_tab = HB[2]; vstart = 0x38; nrec = (dl_tab - vstart) // 16
# ---- each vertex record's bone, from the display lists (G_MTX of segment 3: bone n at n x 0x40)
dls = [struct.unpack('>I', B[dl_tab + 4*i:dl_tab + 4*i + 4])[0] for i in range(HB[3] // 4 if HB[3] else 1)]
rec_bone = [-1] * nrec
mtx_at = []
for dl in dls:
    p = dl; bone = 0
    while p + 8 <= len(B):
        w0, w1 = struct.unpack('>II', B[p:p+8]); op = w0 >> 24
        if op == 0xDF:
            break
        if op == 0xDA and (w1 >> 24) == 3:
            bone = (w1 & 0xFFFFFF) // 0x40; mtx_at.append((p + 4, bone))
        if op == 0x01:
            n = (w0 >> 12) & 0xFF; rec = ((w1 & 0xFFFFFF) - vstart) // 16
            for k in range(n):
                if 0 <= rec + k < nrec:
                    rec_bone[rec + k] = bone
        p += 8
print('records', nrec, 'unbound', rec_bone.count(-1), 'matrix loads', len(mtx_at))
# ---- his bones to 3B's (3B: 0 hips, 1 spine, 2-6 the +x arm (shoulder, elbow, wrist, hand, thumb), 7-11 the -x arm, 12 the head,
# 13 14 the ears, 15 the face, 16-19 the +x leg (hip, knee, ankle, toe), 20-23 the -x leg, 24-27 the tail)
# (his chair, its joysticks and his short legs on 3B's hips: the chair goes where Conker's hips go)
MAP = {0: 0, 1: 0, 30: 0, 31: 0, 28: 0, 29: 0, 2: 1, 3: 1,
       4: 12, 5: 12, 6: 12, 7: 12, 8: 12, 9: 12, 10: 12, 11: 12, 12: 12, 13: 12,
       19: 2, 20: 3, 21: 4, 22: 5, 23: 5, 24: 5, 25: 5, 26: 6, 27: 6,
       14: 7, 15: 8, 16: 9, 17: 10, 18: 11}
SRC = {0: 1, 1: 2, 2: 19, 3: 20, 4: 21, 5: 22, 6: 26, 7: 14, 8: 15, 9: 16, 10: 17, 11: 18, 12: 4, 13: 5, 14: 5, 15: 5,
       16: 28, 17: 28, 18: 28, 19: 28, 20: 29, 21: 29, 22: 29, 23: 29, 24: 1, 25: 1, 26: 1, 27: 1}
# positions in his model (bind pose, world)
P = np.zeros((nrec, 3)); RB = np.zeros(nrec, int)
for i in range(nrec):
    x, y, z = struct.unpack('>3h', B[vstart + 16*i:vstart + 16*i + 6])
    b = max(rec_bone[i], 0); P[i] = np.array([x, y, z], float) + JB[b]; RB[i] = b
# ---- re-pose: his arms (T-pose) turned down about the shoulder to 3B's shoulder-to-wrist direction
def rot(a, c):
    a = a/np.linalg.norm(a); c = c/np.linalg.norm(c); v = np.cross(a, c); s = np.linalg.norm(v); d = np.dot(a, c)
    if s < 1e-9: return np.eye(3)
    K = np.array([[0,-v[2],v[1]],[v[2],0,-v[0]],[-v[1],v[0],0]]); return np.eye(3) + K + K@K*((1-d)/s**2)
JBp = [j.copy() for j in JB]
for sh, wr, chain, tsh, twr in ((19, 21, range(19, 28), 2, 4), (14, 16, range(14, 19), 7, 9)):
    R = rot(JB[wr] - JB[sh], JT[twr] - JT[tsh]); o = JB[sh].copy()
    for b in chain:
        JBp[b] = o + R @ (JB[b] - o)
    m = np.isin(RB, list(chain)); P[m] = (R @ (P[m] - o).T).T + o
# ---- scale: his floor (y 0) to 3B's (-41), his height to HEIGHT
FLOOR_T = -41.0
SCALE = HEIGHT / max(1.0, P[:, 1].max() - P[:, 1].min())
low = P[:, 1].min()
P = (P - [0, low, 0]) * SCALE + [0, FLOOR_T, 0]
JBp = [(j - [0, low, 0]) * SCALE + [0, FLOOR_T, 0] for j in JBp]
print('scale', round(SCALE, 3))
JO = {t: JBp[b].copy() for t, b in SRC.items()}
# ---- write: each record in its new bone's space at his own joints, the matrix loads the new bones, the skeleton 3B's at his joints
for i in range(nrec):
    t = MAP.get(RB[i], 0); v = np.round(P[i] - JO[t]).astype(int)
    B[vstart + 16*i:vstart + 16*i + 6] = struct.pack('>3h', *[int(np.clip(c, -32768, 32767)) for c in v])
for at, bone in mtx_at:
    B[at:at+4] = struct.pack('>I', 0x03000000 | MAP.get(bone, 0) * 0x40)
sk = HB[4]; tsk = struct.unpack('>13I', T[:52])[4]
for i in range(28):
    e = bytearray(T[tsk + 16*i:tsk + 16*i + 16]); par = e[0]
    off = JO[i] - (JO[par] if par != 255 else 0)
    if par == 255:
        off = off - [0, DROP, 0]
    e[4:16] = struct.pack('>3f', *off)
    B[sk + 16*i:sk + 16*i + 16] = e
for i in range(28, len(bb)):
    B[sk + 16*i:sk + 16*i + 16] = bytes(16)
struct.pack_into('>I', B, 20, 28 * 16)
# his runtime slots (segments 6 and 7: a texture at +0, its palette at +0x400), which the story fills (his face's frames): each pointed
# at one of his own textures the list doesn't name otherwise (0x1C47 to 0x1C4E, between the ones it does): 6 the first, 7 the fifth
SLOT_TEX = {6: 0x1C47, 7: 0x1C4B}
slots = 0
for dl in dls:
    p = dl
    while p + 8 <= len(B):
        w0, w1 = struct.unpack('>II', B[p:p+8])
        if w0 >> 24 == 0xDF: break
        if w0 >> 24 == 0xFD and (w1 >> 24) in SLOT_TEX:
            tex = SLOT_TEX[w1 >> 24]
            struct.pack_into('>I', B, p + 4, (0x00400000 | tex) if (w1 & 0xFFFFFF) == 0x400 else tex)
            slots += 1
        p += 8
print('runtime slots pointed at his textures:', slots)
out = bytes(B)
dest = os.path.join(here, '..', '..', 'host', 'assets', 'professor')
os.makedirs(dest, exist_ok=True)
open(os.path.join(dest, 'professor.bin'), 'wb').write(out)
print('written', len(out), 'bytes; height', round(P[:, 1].max() - P[:, 1].min(), 1), 'tail to z', round(min(JO[27][2], P[:, 2].min()), 1))

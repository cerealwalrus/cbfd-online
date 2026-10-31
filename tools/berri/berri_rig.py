# Berri (bank 1 entry 0x72) rebuilt on model 3B's skeleton (the striped character after the teddy: Conker's 28 bones), on this PC
# from its own ROM. Her display list, textures and materials are kept as they are: the skeleton is 3B's, each vertex record put in its
# new bone's space (re-posed: arms down, scaled to 3B's size, limbs laid along 3B's bones), and the G_MTX loads pointed at the new bones.
import struct, zlib, sys, numpy as np
sys.path.insert(0, __file__.rsplit('/',1)[0] if '/' in __file__ else '.')
from models import rom, entry, skeleton, world
r = rom()
B, _ = entry(r, 0x72); T, _ = entry(r, 0x3B)
B = bytearray(B)
HB = list(struct.unpack('>13I', B[:52]))
bb = skeleton(bytes(B)); JB = [np.array(w) for w in world(bb)]
tb = skeleton(T); JT = [np.array(w) for w in world(tb)]
dl_tab = HB[2]; vstart = 0x38; nrec = (dl_tab - vstart) // 16
# ---- each vertex record's bone, from the display lists (G_MTX of segment 3: bone n at n x 0x40)
dls = []
for i in range(HB[3] // 4 if HB[3] else 1):
    dls.append(struct.unpack('>I', B[dl_tab + 4*i:dl_tab + 4*i + 4])[0])
rec_bone = [-1] * nrec
mtx_at = []   # (file offset of the w1 word, bone)
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
# ---- her bones to 3B's
MAP = {0:0, 1:0, 43:0, 44:0, 2:1, 19:1, 20:1, 3:12}
for b in range(4, 19): MAP[b] = 12
for b, t in ((28,2),(29,3),(30,4),(31,5),(32,5),(33,6),(34,6),(21,7),(22,8),(23,9),(24,10),(25,10),(26,11),(27,11),
             (35,16),(36,17),(37,18),(38,18),(39,20),(40,21),(41,22),(42,22)):
    MAP[b] = t
# positions in her model (bind pose, world)
P = np.zeros((nrec, 3)); RB = np.zeros(nrec, int)
for i in range(nrec):
    x, y, z = struct.unpack('>3h', B[vstart + 16*i:vstart + 16*i + 6])
    b = max(rec_bone[i], 0); P[i] = np.array([x, y, z], float) + JB[b]; RB[i] = b
# ---- (asked for) her chest (her bones 19, 20) and hips (43, 44) a little fuller: 15% about their own joints
for b in (19, 20, 43, 44):
    m = RB == b
    P[m] = JB[b] + (P[m] - JB[b]) * 1.15
# ---- re-pose: her arms (T-pose) turned down about the shoulder to 3B's shoulder-to-wrist direction
def rot(a, c):
    a = a/np.linalg.norm(a); c = c/np.linalg.norm(c); v = np.cross(a, c); s = np.linalg.norm(v); d = np.dot(a, c)
    if s < 1e-9: return np.eye(3)
    K = np.array([[0,-v[2],v[1]],[v[2],0,-v[0]],[-v[1],v[0],0]]); return np.eye(3) + K + K@K*((1-d)/s**2)
JBp = [j.copy() for j in JB]
for sh, el, wr, chain, tsh, twr in ((28,29,30,range(28,35),2,4), (21,22,23,range(21,28),7,9)):
    R = rot(JB[wr] - JB[sh], JT[twr] - JT[tsh]); o = JB[sh].copy()
    for b in chain:
        JBp[b] = o + R @ (JB[b] - o)
    m = np.isin(RB, list(chain)); P[m] = (R @ (P[m] - o).T).T + o
# ---- scale: her floor (y 0) to 3B's (-41), her height to 3B's (head top: 3B's ears at 141, hers at about 330)
FLOOR_B, FLOOR_T = 0.0, -41.0
SCALE = float(sys.argv[1]) if len(sys.argv) > 1 else 0.56
P = (P - [0, FLOOR_B, 0]) * SCALE + [0, FLOOR_T, 0]
JBp = [(j - [0, FLOOR_B, 0]) * SCALE + [0, FLOOR_T, 0] for j in JBp]
# ---- (--own) her own proportions: 3B's 28 bones (the same order and parents, which Conker's animations need) placed at her own
# joints, nothing moved onto 3B's; the file's skeleton hers
OWN = '--own' in sys.argv
# (--drop=N) her skeleton's root lowered by N (her units after the scale, 3B's): with Conker's animations her feet stood above the floor
# (the 30 Oct video: about 12 at 0.62)
DROP = next((float(a.split('=')[1]) for a in sys.argv if a.startswith('--drop=')), 12.0)
if OWN:
    SRC = {0:1, 1:2, 2:28, 3:29, 4:30, 5:31, 6:33, 7:21, 8:22, 9:23, 10:24, 11:26, 12:3, 13:17, 14:18, 15:15,
           16:35, 17:36, 18:37, 19:38, 20:39, 21:40, 22:41, 23:42, 24:1, 25:1, 26:1, 27:1}
    JO = {t: JBp[b].copy() for t, b in SRC.items()}
    for i in range(nrec):
        t = MAP.get(RB[i], 0); v = np.round(P[i] - JO[t]).astype(int)
        B[vstart + 16*i:vstart + 16*i + 6] = struct.pack('>3h', *[int(np.clip(c, -32768, 32767)) for c in v])
    orig_bone_at = {at: bone for at, bone in mtx_at}
    for at, bone in mtx_at:
        B[at:at+4] = struct.pack('>I', 0x03000000 | MAP.get(bone, 0) * 0x40)
    sk = HB[4]; tsk = struct.unpack('>13I', T[:52])[4]
    for i in range(28):
        e = bytearray(T[tsk + 16*i:tsk + 16*i + 16]); par = e[0]
        off = JO[i] - (JO[par] if par != 255 else 0)
        if par == 255:
            off = off - [0, DROP, 0]   # (her whole skeleton lowered: her feet were off the floor in the game)
        e[4:16] = struct.pack('>3f', *off)
        B[sk + 16*i:sk + 16*i + 16] = e
    for i in range(28, len(bb)):
        B[sk + 16*i:sk + 16*i + 16] = bytes(16)
    struct.pack_into('>I', B, 20, 28 * 16)
    SKIP_REST = True
else:
    SKIP_REST = False
# ---- (--natural) her own proportions on 3B's bones: her body, neck and head as they are (no stretch), her arms and legs at her own
# lengths along 3B's bone directions from 3B's shoulders and hips, the whole of her raised so her feet stand on 3B's floor
NATURAL = '--natural' in sys.argv
# ---- each part to 3B's joint: limbs laid along 3B's bones (their lengths), the rest moved with its joint
CHILD_B = {28:29, 29:30, 30:31, 21:22, 22:23, 23:24, 35:36, 36:37, 39:40, 40:41}
DELTA = ((JT[16] - JBp[35]) + (JT[20] - JBp[39])) * 0.5
NAT_START = {}; NAT_END = {}
if '--natural' in sys.argv:
    # where each of her chain joints goes: from 3B's first joint of the chain, each segment along 3B's direction at her length
    for chain in ((28, 29, 30, 31), (21, 22, 23, 24), (35, 36, 37, 38), (39, 40, 41, 42)):
        pos = JT[MAP[chain[0]]].copy()
        for j in range(len(chain) - 1):
            b, c = chain[j], chain[j+1]
            NAT_START[b] = pos.copy()
            td = JT[MAP[c]] - JT[MAP[b]]
            if np.linalg.norm(td) < 1e-6: break
            pos = pos + td / np.linalg.norm(td) * np.linalg.norm(JBp[c] - JBp[b])
        NAT_END[chain[-1]] = pos.copy(); NAT_END[chain[-2] if False else chain[-1]] = pos.copy()
    # hands and thumbs, feet and toes: with the end of their chain
    for b in (25, 26, 27): NAT_END[b] = NAT_START.get(24, JT[MAP[b]])
    for b in (32, 33, 34): NAT_END[b] = NAT_START.get(31, JT[MAP[b]])
    NAT_END[38] = NAT_START.get(37, NAT_END.get(38)); NAT_END[42] = NAT_START.get(41, NAT_END.get(42))
    NAT_END[37] = NAT_START.get(37); NAT_END[41] = NAT_START.get(41)
HIP_Y = ((JBp[35] + JBp[39]) * 0.5 + DELTA)[1]; SH_Y = ((JBp[28] + JBp[21]) * 0.5 + DELTA)[1]
print('body moved by', np.round(DELTA,1), '; her shoulders then at', np.round(JBp[28]+DELTA,1), np.round(JBp[21]+DELTA,1), '3B', np.round(JT[2],1), np.round(JT[7],1), '; her neck', np.round(JBp[3]+DELTA,1), '3B head joint', np.round(JT[12],1))
for i in (range(nrec) if not SKIP_REST else []):
    b = RB[i]; t = MAP.get(b, 0)
    if b in CHILD_B:
        c = CHILD_B[b]; tc = MAP[c]
        a = JBp[b]; d = JBp[c] - a; L = float(np.dot(d, d))
        ta = JT[t]; td = JT[tc] - ta
        if NATURAL:
            td = td / np.linalg.norm(td) * np.sqrt(L)   # (her own length)
            ta = ta if b not in (35, 39, 21, 28) else ta
            ta = NAT_START.get(b, ta)
        k = float(np.dot(P[i] - a, d) / L); perp = P[i] - a - k*d
        P[i] = ta + k*td + rot(d, td) @ perp
    elif t in (0, 1, 12):
        # (her body, head and hair: moved together so her hips meet 3B's, and from her hips to her shoulders stretched up to 3B's
        # shoulders, so her arms come out of her shoulders; her head on top as it is)
        q = P[i] + DELTA
        hip = HIP_Y; sh = SH_Y
        if q[1] > hip and not NATURAL:
            if q[1] <= sh:
                q[1] = JT[16][1] + (q[1] - hip) * (JT[2][1] - JT[16][1]) / (sh - hip)
            else:
                q[1] = q[1] + (JT[2][1] - sh)
        P[i] = q
    else:
        P[i] = P[i] - JBp[b] + (NAT_END.get(b, JT[t]) if NATURAL else JT[t])
# feet on 3B's floor
if NATURAL and not SKIP_REST:
    fb = np.isin(RB, [37, 38, 41, 42])
    P[:, 1] += (min(JT[19][1], JT[23][1]) - 6.0) - P[fb, 1].min()
feet = (not SKIP_REST) & (not NATURAL) & np.isin([MAP.get(b, 0) for b in RB], [18, 22])
if feet.any():
    P[feet, 1] += (min(JT[19][1], JT[23][1]) - 6.0) - P[feet, 1].min()
# ---- write: records in their new bone's space, the skeleton 3B's, the matrix loads the new bones
for i in (range(nrec) if not SKIP_REST else []):
    t = MAP.get(RB[i], 0); v = np.round(P[i] - JT[t]).astype(int)
    B[vstart + 16*i:vstart + 16*i + 6] = struct.pack('>3h', *[int(np.clip(c, -32768, 32767)) for c in v])
if not SKIP_REST:
    orig_bone_at = {at: bone for at, bone in mtx_at}
for at, bone in (mtx_at if not SKIP_REST else []):
    B[at:at+4] = struct.pack('>I', 0x03000000 | MAP.get(bone, 0) * 0x40)
sk = HB[4]
tsk = struct.unpack('>13I', T[:52])[4]
for i in (range(28) if not SKIP_REST else []):
    B[sk + 16*i:sk + 16*i + 16] = T[tsk + 16*i:tsk + 16*i + 16]
if not SKIP_REST:
    for i in range(28, len(bb)):
        B[sk + 16*i:sk + 16*i + 16] = bytes(16)
    struct.pack_into('>I', B, 20, 28 * 16)
# her runtime slots (segments 6 and 7: a texture at +0, its palette at +0x800), which the game fills for her in the story: each pointed
# at her own texture by what it's drawn on (the next matrix loaded's bone): her face's open-eyes sheet (3C8) on her head, her shoes'
# (3CB) on her feet
eyes = 0
for dl in dls:
    p = dl; cmds = []
    while p + 8 <= len(B):
        w0, w1 = struct.unpack('>II', B[p:p+8]); cmds.append((p, w0, w1))
        if w0 >> 24 == 0xDF: break
        p += 8
    for i, (p, w0, w1) in enumerate(cmds):
        if w0 >> 24 != 0xFD or (w1 >> 24) not in (6, 7):
            continue
        # the bone the texture is drawn on: the matrix in force at the next vertex load (the original's bone numbers)
        bone = None; cur = None
        for q, a, b in cmds[:i]:
            if a >> 24 == 0xDA and (b >> 24) == 3: cur = orig_bone_at.get(q + 4, cur)
        for q, a, b in cmds[i:]:
            if a >> 24 == 0xDA and (b >> 24) == 3: cur = orig_bone_at.get(q + 4, cur)
            if a >> 24 == 0x01: bone = cur; break
        tex = 0x3CB if bone is not None and bone >= 35 else 0x3C8
        struct.pack_into('>I', B, p + 4, (0x00400000 | tex) if (w1 & 0xFFFFFF) == 0x800 else tex)
        eyes += 1
        print('slot', w1 >> 24, hex(w1), 'drawn on her bone', bone, '->', hex(tex))
print('eye loads', eyes)
out = bytes(B)
c = zlib.compressobj(9, zlib.DEFLATED, -15); z = c.compress(out) + c.flush()
open('tools/berri/berri.rzip', 'wb').write(struct.pack('>I', len(out)) + z)
open('host/assets/berri/berri_own.bin' if OWN else ('host/assets/berri/berri_natural.bin' if NATURAL else 'host/assets/berri/berri.bin'), 'wb').write(out)
np.save('tools/berri/P.npy', P)
print('written', len(out), 'bytes,', len(z) + 4, 'packed; height', round(P[:,1].max() - P[:,1].min(), 1))

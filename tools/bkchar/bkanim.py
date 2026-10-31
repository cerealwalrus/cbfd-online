# Banjo-Kazooie animations (as core2/animationfile.c reads them) on a skeleton (a model's animation list, as code_630D0.c's
# animMtxList_setBoned builds its matrices). Column vectors: M_i = M_parent . T(p + k t) . Rz Ry Rx . S . T(-p).
import struct, math
import numpy as np

def load_anim(b):
    start, end, n = struct.unpack('>hhh', b[:6])
    at = 8
    els = []
    for _ in range(n):
        w, cnt = struct.unpack('>Hh', b[at:at + 4]); at += 4
        bone, typ = w >> 4, w & 0xF
        keys = []
        for _ in range(cnt):
            f, v = struct.unpack('>Hh', b[at:at + 4]); at += 4
            keys.append(((f >> 15) & 1, (f >> 14) & 1, f & 0x3FFF, v))
        els.append((bone, typ, keys))
    return dict(start=start, end=end, els=els)

DEF = [0, 0, 0, 1, 1, 1, 0, 0, 0]
def cr(x, k):
    x = min(1.0, max(0.0, x))
    a2 = -0.5 * k[0] + 1.5 * k[1] - 1.5 * k[2] + 0.5 * k[3]
    a1 = 1.0 * k[0] - 2.5 * k[1] + 2.0 * k[2] - 0.5 * k[3]
    a0 = -0.5 * k[0] + 0.5 * k[2]
    return ((a2 * x + a1) * x + a0) * x + k[1]

def el_value(A, typ, keys, time):
    first = 0; last = len(keys) - 1
    K = keys
    if int(time) < K[0][2]:
        kn = [DEF[typ], DEF[typ], K[0][3] / 64.0, 0]
        kn[3] = K[1][3] / 64.0 if (K[0][0] == 1 and len(K) >= 2) else kn[2]
        return cr((time - A['start']) / (K[0][2] - A['start']), kn)
    if int(time) >= K[last][2]:
        k1 = K[last][3] / 64.0
        k0 = K[last - 1][3] / 64.0 if (K[last][1] == 1 and len(K) >= 2) else k1
        return cr(time - K[last][2], [k0, k1, k1, k1])
    lo, hi = 0, last
    i = lo + 1
    while i < hi:
        i = lo + (hi - lo) // 2
        if K[i][2] <= int(time):
            lo = i
        else:
            hi = i
        i = lo + 1
    k1 = K[lo][3] / 64.0; k2 = K[hi][3] / 64.0
    t = (time - K[lo][2]) / (K[hi][2] - K[lo][2])
    if K[lo][1] == 0 and K[hi][0] == 0:
        return k1 + (k2 - k1) * t
    k0 = K[lo - 1][3] / 64.0 if (K[lo][1] == 1 and lo - 1 >= 0) else k1
    k3 = K[hi + 1][3] / 64.0 if (K[hi][0] == 1 and hi + 1 < len(K)) else k2
    return cr(t, [k0, k1, k2, k3])

def transforms(A, progress):
    """bone id -> 9 values (rx ry rz deg, sx sy sz, tx ty tz)"""
    time = A['start'] + progress * (A['end'] - A['start'])
    out = {}
    for bone, typ, keys in A['els']:
        v = out.setdefault(bone, list(DEF))
        v[typ] = el_value(A, typ, keys, time)
    return out

def rot(rx, ry, rz):
    d = math.pi / 180.0
    cx, sx = math.cos(rx * d), math.sin(rx * d)
    cy, sy = math.cos(ry * d), math.sin(ry * d)
    cz, sz = math.cos(rz * d), math.sin(rz * d)
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return Rz @ Ry @ Rx

def load_skeleton(model_bytes):
    m = model_bytes
    a = struct.unpack('>I', m[0x18:0x1C])[0]
    k, n = struct.unpack('>fh', m[a:a + 6])
    S = []
    for i in range(n):
        x, y, z, bid, par = struct.unpack('>fffhh', m[a + 8 + 16 * i:a + 24 + 16 * i])
        S.append((np.array([x, y, z]), bid, par))
    return k, S

def matrices(k, S, tr):
    """4x4 column-vector world matrices for each bone of skeleton S given transforms tr (bone id -> 9 values)"""
    M = []
    for p, bid, par in S:
        v = tr.get(bid, DEF)
        L = np.eye(4)
        R = rot(v[0], v[1], v[2]) @ np.diag([v[3], v[4], v[5]])
        L[:3, :3] = R
        t = p + k * np.array(v[6:9])
        L[:3, 3] = t - R @ p
        M.append((M[par] if par >= 0 else np.eye(4)) @ L)
    return M

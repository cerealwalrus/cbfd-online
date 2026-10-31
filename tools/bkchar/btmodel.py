# Banjo-Tooie model (0x0B) -> triangles bound to bones, as the N64 would draw them: the geo layout walked with a stack of bone
# matrices, each display list run through a vertex cache (each slot remembers the bone current when it was loaded; G_POPMTX in a
# skinning list (0x18) goes back to the enclosing bone), the texture of each triangle from the G_SETTIMG/LOADBLOCK in force.
import struct, sys
import numpy as np

class BT:
    def __init__(self, path, selectors=None):
        m = self.m = open(path, 'rb').read()
        self.geo = struct.unpack('>I', m[4:8])[0]
        self.tex_at = struct.unpack('>H', m[8:10])[0]
        self.gfx_at = struct.unpack('>I', m[0xC:0x10])[0]
        self.vtx_at = struct.unpack('>I', m[0x10:0x14])[0]
        self.bones_at = struct.unpack('>I', m[0x18:0x1C])[0]
        self.nrm_at = struct.unpack('>I', m[0x38:0x3C])[0]
        self.ntri, self.nvtx = struct.unpack('>HH', m[0x44:0x48])
        # textures
        size, cnt = struct.unpack('>iH', m[self.tex_at:self.tex_at + 6])
        self.tex_data = self.tex_at + 8 + 8 * cnt
        self.textures = []
        for i in range(cnt):
            off, typ, w, h = struct.unpack('>iHBB', m[self.tex_at + 8 + 8 * i:self.tex_at + 16 + 8 * i])
            self.textures.append(dict(off=off, type=typ, w=w, h=h))
        # gfx
        ng = struct.unpack('>I', m[self.gfx_at:self.gfx_at + 4])[0]
        self.G = [struct.unpack('>II', m[self.gfx_at + 8 + 8 * i:self.gfx_at + 16 + 8 * i]) for i in range(ng)]
        # vertices (+ normals)
        v0 = self.vtx_at + 0x18
        self.V = []
        for i in range(self.nvtx):
            x, y, z, fl, s, t, r, g, b, a = struct.unpack('>hhhHhhBBBB', m[v0 + 16 * i:v0 + 16 * i + 16])
            nx, ny, nz = struct.unpack('>bbb', m[self.nrm_at + 4 * i:self.nrm_at + 4 * i + 3]) if self.nrm_at else (0, 0, 0)
            self.V.append((x, y, z, s, t, r, g, b, a, nx, ny, nz))
        # bones
        sc, nb = struct.unpack('>fh', m[self.bones_at:self.bones_at + 6])
        self.bone_scale = sc
        self.bones = []
        for i in range(nb):
            x, y, z, bid, par = struct.unpack('>fffhh', m[self.bones_at + 8 + 16 * i:self.bones_at + 24 + 16 * i])
            self.bones.append(dict(p=(x, y, z), id=bid, par=par))
        self.selectors = selectors or {}
        self.tris = []      # (texture index or -1, [(vertex index, bone) x3], combiner)
        self.unknown = {}
        self.stack = []
        self.walk(self.geo)

    def walk(self, at):
        m = self.m
        while True:
            cmd, nxt = struct.unpack('>Ii', m[at:at + 8])
            if cmd == 0x02:
                br, mtx, bid = struct.unpack('>Bbh', m[at + 8:at + 12])
                self.stack.append(mtx)
                if br:
                    self.walk(at + br)
                self.stack.pop()
            elif cmd == 0x11:
                gi, vs, vc = struct.unpack('>hhh', m[at + 8:at + 14])
                self.run_dl(gi, skin=False)
            elif cmd == 0x18:
                gi = struct.unpack('>h', m[at + 8:at + 10])[0]
                self.run_dl(gi, skin=True)
            elif cmd == 0x0C:
                n, idx = struct.unpack('>hh', m[at + 8:at + 12])
                offs = struct.unpack('>%di' % n, m[at + 12:at + 12 + 4 * n])
                sel = self.selectors.get(idx, 1)
                if 1 <= sel <= n:
                    self.walk(at + offs[sel - 1])
            elif cmd == 0x0D:
                br = struct.unpack('>h', m[at + 0x14:at + 0x16])[0]
                if br:
                    self.walk(at + br)
            elif cmd == 0x0A:
                pass
            else:
                self.unknown[cmd] = self.unknown.get(cmd, 0) + 1
            if not nxt:
                break
            at += nxt

    def run_dl(self, gi, skin):
        cur = self.stack[-1] if self.stack else 0
        parent = self.stack[-2] if len(self.stack) > 1 else cur
        slot = [None] * 64
        tex_img = None; pal = None; tex = -1
        comb = None
        i = gi
        while True:
            w0, w1 = self.G[i]
            op = w0 >> 24
            if op == 0xDF:
                break
            if op == 0x01:
                n = (w0 >> 12) & 0xFF; end = (w0 & 0xFF) >> 1
                seg = w1 >> 24; off = w1 & 0xFFFFFF
                assert seg == 1, hex(w1)
                first = off // 16
                for k in range(n):
                    slot[end - n + k] = (first + k, cur)
            elif op == 0xD8:
                cur = parent
            elif op == 0x05:
                a, b, c = ((w0 >> 16) & 0xFF) // 2, ((w0 >> 8) & 0xFF) // 2, (w0 & 0xFF) // 2
                self.tris.append((tex, [slot[a], slot[b], slot[c]], comb))
            elif op == 0x06:
                for w in (w0, w1):
                    a, b, c = ((w >> 16) & 0xFF) // 2, ((w >> 8) & 0xFF) // 2, (w & 0xFF) // 2
                    self.tris.append((tex, [slot[a], slot[b], slot[c]], comb))
            elif op == 0xFD:
                seg = w1 >> 24; off = w1 & 0xFFFFFF
                fmt = (w0 >> 21) & 7
                if seg == 2:
                    if fmt == 0:      # the palette (RGBA16 for LOADTLUT)
                        pal = off
                    else:
                        tex_img = off
            elif op == 0xF3 or op == 0xF0:
                # texture picked by its texels' (or palette's) place
                for k, t in enumerate(self.textures):
                    if t['type'] in (1, 2):
                        palsize = 0x20 if t['type'] == 1 else 0x200
                        if tex_img is not None and tex_img == t['off'] + palsize:
                            tex = k
                    elif tex_img is not None and tex_img == t['off']:
                        tex = k
            elif op == 0xD7:
                if (w1 == 0) or (w0 & 0xFF) == 0 and False:
                    pass
                if (w0 & 0x2) == 0:   # texture off
                    tex = -1 if False else tex
            elif op == 0xFC:
                comb = (w0, w1)
            elif op == 0xDE:
                pass
            i += 1

if __name__ == '__main__':
    bt = BT(sys.argv[1])
    print('verts', len(bt.V), 'tris', len(bt.tris), 'unknown cmds', bt.unknown)
    bad = sum(1 for t in bt.tris if None in t[1])
    print('tris with an unloaded slot', bad)
    from collections import Counter
    print('by bone', sorted(Counter(c[1] for t in bt.tris for c in t[1] if c).items()))
    print('by texture', sorted(Counter(t[0] for t in bt.tris).items()))
    print('combiners', Counter('%08X %08X' % t[2] for t in bt.tris if t[2]))

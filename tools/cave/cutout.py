#!/usr/bin/env python3
"""
(0.11.4) See-through textures on the maps made from meshes (tools/cave/cave_level.py): cut out, not drawn black.

  python3 -I tools/cave/cutout.py host/assets/<map> [host/assets/<map> ...]

A texture with see-through texels (fences, window tracery, statues, leaves, splashes: the picture's alpha under half) has them at
palette index 0, which was black with no alpha bit, and its material was drawn with the maps' opaque render mode, so they came out
black. For each such texture (index 0 among its texels):
  - its material's othermode (EF08AC3F 00552230 in the map's display list) gets CVG_X_ALPHA (00553230): the coverage times the
    texel's alpha, so a texel with no alpha isn't drawn (RT64: RasterPS.hlsl's resultCvg);
  - its palette's index 0 becomes the texture's average colour (still no alpha bit), so filtering at a cut edge blends towards the
    texture's own colour, not black.
The map's geometry is untouched: cave.rzip is unpacked, those words changed, and packed again. Run again, it changes nothing more.
"""
import sys, os, struct, zlib, glob

def c16(r, g, b):
    return ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1)

for folder in sys.argv[1:]:
    see = {}
    for f in sorted(glob.glob(os.path.join(folder, 'tex_*.bin'))):
        tid = int(os.path.basename(f)[4:8], 16)
        d = bytearray(open(f, 'rb').read())
        n = len(d) - 512
        texels = d[:n]
        if 0 not in texels:
            continue
        pal = struct.unpack('>256H', bytes(d[n:n + 512]))
        counts = [0] * 256
        for t in texels:
            counts[t] += 1
        tot = sum(counts[1:])
        if tot:
            acc = [0.0, 0.0, 0.0]
            for i in range(1, 256):
                if counts[i]:
                    p = pal[i]
                    acc[0] += ((p >> 11) & 31) * counts[i]; acc[1] += ((p >> 6) & 31) * counts[i]; acc[2] += ((p >> 1) & 31) * counts[i]
            r, g, b = [int(round(a / tot)) << 3 for a in acc]
            d[n:n + 2] = struct.pack('>H', c16(r, g, b))
            open(f, 'wb').write(bytes(d))
        see[tid] = (100 * texels.count(0)) // n
    path = os.path.join(folder, 'cave.rzip')
    rz = open(path, 'rb').read()
    size = struct.unpack('>I', rz[:4])[0]
    e = bytearray(zlib.decompressobj(wbits=-15).decompress(rz[4:]))
    assert len(e) == size, (folder, len(e), size)
    w = lambda a: struct.unpack('>I', bytes(e[a:a + 4]))[0]
    c = w(0)
    dl = c + w(c)
    dl_len = w(c + 4)
    tid = None
    patched = 0
    for a in range(dl, dl + dl_len, 8):
        w0, w1 = w(a), w(a + 4)
        if w0 == 0xFD500000:
            tid = w1 & 0xFFFF
        elif w0 == 0xEF08AC3F and (w1 & ~0x1000) == 0x00552230 and tid in see:
            if w1 != 0x00553230:
                e[a + 4:a + 8] = struct.pack('>I', 0x00553230)
                patched += 1
    co = zlib.compressobj(9, zlib.DEFLATED, -15)
    out = struct.pack('>I', len(e)) + co.compress(bytes(e)) + co.flush()
    open(path, 'wb').write(out)
    print('%s: %d see-through textures (%s), %d materials cut out; cave.rzip %d bytes packed (was %d)' % (
        folder, len(see), ' '.join('%04X:%d%%' % (k, v) for k, v in sorted(see.items())), patched, len(out), len(rz)))

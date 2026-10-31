#!/usr/bin/env python3
"""
The textures of the characters and maps made from meshes (tools/objchar/obj_character.py, tools/cave/cave_level.py) at their own
full size, as an RT64 texture pack the game matches by Rice hash as it loads them (recomp/rt64.patch, rt64_rice_hash.cpp).

In the game each of those textures is a CI8 one of at most 2048 texels (64 x 32: the most the Nintendo 64's texture memory takes
with a 256-colour palette), so a 512 x 512 skin is squeezed to 64 x 32. This pack puts each one's own picture back over it, at
whatever size it was made, without changing the game's data:

  python3 -I tools/texture_packs/custom_hd_pack.py --out FOLDER [--rtz FILE] ASSETS=SOURCES[@GAMMA] ...

ASSETS is a folder of the game's textures (tex_XXXX.bin: the texels, the odd rows' halves swapped as loaded, then 256 RGBA5551
colours), SOURCES the folder its mesh came from (its pictures, any depth), GAMMA the lift cave_level.py gave a map's pictures
(--gamma, 0.6 by default; 1 for characters). Each texture is matched to the picture it was made from (the picture made as small,
the closest), and named for its Rice hash as the game loads it:
  - the texels' hash: RiceCRC32 over the width x height texels (8-bit, a row being its width in bytes, as LoadBlock with dxt 0 and
    the tile's line give), read as RT64 reads RDRAM (its 32-bit words in the machine's order);
  - the palette's: RiceCRC32 over the colours up to the highest index used;
  - the format of the tile it's drawn with: the characters' 0 (RGBA, its palette by the render mode), the maps' 2 (CI); 8-bit.
The pack: those pictures, named "CONKER BFD#<texels>#<format>#1#<palette>_all.png", an rt64.json with the Rice auto path, and a
mod.json; with --rtz, the same zipped, to go in the mods folder (or in host/assets/packs, for the game to put there itself).
"""
import argparse, glob, json, os, re, struct, sys, zipfile
import numpy as np
from PIL import Image

ROM_NAME = "CONKER BFD"


def pow2(n):
    p = 1
    while p < n:
        p *= 2
    return p


def size_for(img):
    w, h = max(8, min(64, pow2(img.size[0]))), max(8, min(64, pow2(img.size[1])))
    while w * h > 2048:
        if w >= h:
            w //= 2
        else:
            h //= 2
    return w, h


def decode(data, w, h):
    """A tex_XXXX.bin as RGBA (w x h), and its texels as stored (the odd rows' halves swapped) and its palette's bytes."""
    n = w * h
    px = np.frombuffer(data[:n], np.uint8).reshape(h, w).copy()
    stored = px.copy()
    for y in range(1, h, 2):
        px[y] = px[y].reshape(-1, 2, 4)[:, ::-1, :].reshape(-1)
    pal = struct.unpack('>256H', data[n:n + 512])
    rgba = np.zeros((256, 4), np.uint8)
    for i, c in enumerate(pal):
        rgba[i] = (((c >> 11) & 31) * 255 // 31, ((c >> 6) & 31) * 255 // 31, ((c >> 1) & 31) * 255 // 31, 255 if c & 1 else 0)
    return rgba[px], stored.tobytes(), data[n:n + 512]


def host_u32(b, a):
    """4 bytes at a, as RT64 reads them from the recompiled game's RDRAM (each 32-bit word stored in the machine's byte order)."""
    return b[a ^ 3] | (b[(a + 1) ^ 3] << 8) | (b[(a + 2) ^ 3] << 16) | (b[(a + 3) ^ 3] << 24)


def rice_crc32(b, width, height, size, row_stride):
    crc = 0
    bytes_per_line = (width << size) >> 1
    y = height - 1
    off = 0
    while y >= 0:
        esi = 0
        x = bytes_per_line - 4
        while x >= 0:
            esi = host_u32(b, off + x) ^ x
            crc = ((crc << 4) + ((crc >> 28) & 15)) & 0xFFFFFFFF
            crc = (crc + esi) & 0xFFFFFFFF
            x -= 4
        esi ^= y
        crc = (crc + esi) & 0xFFFFFFFF
        off += row_stride
        y -= 1
    return crc


def rice_name(texels, palette, w, h, fmt):
    crc = rice_crc32(texels, w, h, 1, w)
    cimax = max(texels[:w * h]) if texels else 0
    pal_crc = rice_crc32(palette + bytes(4), cimax + 1, 1, 2, 512)
    return "%s#%08X#%d#1#%08X_all.png" % (ROM_NAME, crc, fmt, pal_crc)


def pictures(folder):
    out = []
    for ext in ('png', 'jpg', 'jpeg', 'bmp', 'tga'):
        out += glob.glob(os.path.join(folder, '**', '*.' + ext), recursive=True)
        out += glob.glob(os.path.join(folder, '**', '*.' + ext.upper()), recursive=True)
    return sorted(set(out))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[1])
    ap.add_argument('--out', required=True)
    ap.add_argument('--rtz')
    ap.add_argument('--id', default='conker_custom_hd')
    ap.add_argument('--name', default='Imported characters and maps in full detail')
    ap.add_argument('--author', default='Matt')
    ap.add_argument('items', nargs='+')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for name in os.listdir(a.out):
        if name.lower().endswith('.png') or name in ('rt64.json', 'mod.json'):
            os.remove(os.path.join(a.out, name))
    written = {}
    for item in a.items:
        m = re.match(r'^(.*?)=(.*?)(?:@([0-9.]+))?$', item)
        assets, sources, gamma = m.group(1), m.group(2), float(m.group(3) or 1.0)
        fmt = 2 if gamma != 1.0 else 0
        pics = []
        for p in pictures(sources):
            try:
                im = Image.open(p).convert('RGBA')
            except Exception:
                continue
            if gamma != 1.0:
                arr = np.asarray(im).astype(np.float64)
                arr[:, :, :3] = 255.0 * (arr[:, :, :3] / 255.0) ** gamma
                im = Image.fromarray(arr.clip(0, 255).astype(np.uint8), 'RGBA')
            pics.append((p, im))
        found = 0
        for tf in sorted(glob.glob(os.path.join(assets, 'tex_*.bin'))):
            data = open(tf, 'rb').read()
            n = len(data) - 512
            best = None
            for p, im in pics:
                w, h = size_for(im)
                if w * h != n:
                    continue
                rgba, texels, palette = decode(data, w, h)
                small = np.asarray(im.resize((w, h), Image.LANCZOS)).astype(np.float64)
                err = float(np.mean(np.abs(small[:, :, :3] - rgba[:, :, :3].astype(np.float64)) * (rgba[:, :, 3:4] > 0)))
                if best is None or err < best[0]:
                    best = (err, p, im, w, h, texels, palette)
            if best is None or best[0] > 40:
                print('  %s: no picture of its own found%s' % (tf, '' if best is None else ' (closest %.0f off)' % best[0]))
                continue
            err, p, im, w, h, texels, palette = best
            name = rice_name(texels, palette, w, h, fmt)
            if name in written:
                continue
            im.save(os.path.join(a.out, name))
            written[name] = (tf, p, w, h, err)
            found += 1
        print('%s: %d of %d textures from %s' % (assets, found, len(glob.glob(os.path.join(assets, 'tex_*.bin'))), sources))
    db = {"configuration": {"autoPath": "rice", "configurationVersion": 3, "hashVersion": 5}, "textures": [],
          "operationFilters": [], "shiftFilters": [], "extraFiles": []}
    json.dump(db, open(os.path.join(a.out, 'rt64.json'), 'w'), indent=4)
    manifest = {"game_id": "conker", "id": a.id, "version": "1.0.0", "display_name": a.name,
                "description": "The characters and maps made from meshes with their own textures at the size they were made: in the "
                               "game each is at most 64 x 32 (all a 256-colour texture can be), this pack puts the original picture over it.",
                "authors": [a.author], "minimum_recomp_version": "0.1.0"}
    json.dump(manifest, open(os.path.join(a.out, 'mod.json'), 'w'), indent=4)
    if a.rtz:
        with zipfile.ZipFile(a.rtz, 'w', zipfile.ZIP_STORED) as z:
            z.write(os.path.join(a.out, 'mod.json'), 'mod.json')
            z.write(os.path.join(a.out, 'rt64.json'), 'rt64.json')
            for name in sorted(written):
                z.write(os.path.join(a.out, name), name)
    print('%d pictures; %s' % (len(written), a.rtz or a.out))


if __name__ == '__main__':
    main()

# Banjo-Kazooie (USA) ROM's assets: the table at 0x5E90 (count, then each: offset, compressed, flags), the data after it; a
# compressed one is rarezip (0x11 0x72, its size, then deflate).
import struct, zlib

class Assets:
    def __init__(self, path):
        self.r = open(path, 'rb').read()
        if self.r[:4] == b'\x37\x80\x40\x12':        # (.v64: byte-swapped)
            b = bytearray(self.r)
            b[0::2], b[1::2] = self.r[1::2], self.r[0::2]
            self.r = bytes(b)
        assert self.r[0x20:0x2D] == b'Banjo-Kazooie', 'not Banjo-Kazooie'
        A = 0x5E90
        self.count = struct.unpack('>I', self.r[A:A + 4])[0]
        self.ents = [struct.unpack('>IHH', self.r[A + 8 + 8 * i:A + 16 + 8 * i]) for i in range(self.count)]
        self.data0 = A + 8 + 8 * self.count

    def get(self, i):
        off, comp, flags = self.ents[i]
        nxt = self.ents[i + 1][0] if i + 1 < self.count else off
        raw = self.r[self.data0 + off:self.data0 + nxt]
        if not raw:
            return b''
        if comp:
            assert raw[:2] == b'\x11\x72', 'asset %X: not rarezip' % i
            size = struct.unpack('>I', raw[2:6])[0]
            return zlib.decompressobj(-15).decompress(raw[6:])[:size]
        return raw

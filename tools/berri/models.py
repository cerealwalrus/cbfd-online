# Reads model entries of bank 1 from the ROM (on this PC only): rzip = 4-byte length + raw deflate.
import struct, zlib, sys
ROM='conker/baserom.us.z64'
BANK1=0xAF4918
def rom(): return open(ROM,'rb').read()
def entry(r, e):
    off,flags=struct.unpack('>II',r[BANK1+8*e:BANK1+8*e+8])
    size=flags&0x0FFFFFFF
    blk=r[BANK1+off:BANK1+off+size]
    return zlib.decompressobj(wbits=-15).decompress(blk[4:]), blk
def skeleton(d):
    H=struct.unpack('>13I',d[:52]); skel=H[4]; n=H[5]//16; bones=[]
    for i in range(n):
        e=d[skel+16*i:skel+16*i+16]
        bones.append((e[0],e[1],e[2],e[3],struct.unpack('>3f',e[4:16])))
    return bones
def world(bones):
    W=[]
    for i,(p,_,_,_,o) in enumerate(bones):
        base=W[p] if p!=255 else (0,0,0)
        W.append(tuple(base[k]+o[k] for k in range(3)))
    return W
if __name__=='__main__':
    r=rom()
    for e in [int(x,16) for x in sys.argv[1:]]:
        d,blk=entry(r,e); H=struct.unpack('>13I',d[:52])
        b=skeleton(d); W=world(b)
        print('entry %02X: %d bytes (%d packed), header %s, %d bones'%(e,len(d),len(blk),' '.join('%X'%h for h in H),len(b)))
        for i,(bb,w) in enumerate(zip(b,W)):
            print('  %2d parent %3d  ids %02X %02X %02X  at %7.1f %7.1f %7.1f'%(i,bb[0],bb[1],bb[2],bb[3],*w))

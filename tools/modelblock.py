"""Read SWE1R model blocks (N64 ROM or PC out_modelblock.bin) and count geometry.

Usage: python3 tools/modelblock.py baserom.z64 0x141e200   (N64; PC: <out_modelblock.bin> 0)
N64 "Comp" models are LZSS-compressed; decomp() is a port of func_80011940.
"""
import struct, sys
def u32(d,o): return struct.unpack_from('>I',d,o)[0]
def u16(d,o): return struct.unpack_from('>H',d,o)[0]
def load(path, base):
    d = open(path,'rb').read()[base:]
    n = u32(d,0)
    offs = struct.unpack_from('>%dI'%(2*n+1), d, 4)
    return d, [(offs[2*i+1], offs[2*i+2]) for i in range(n)]
def roots(m):
    out=[]; o=4
    while o+4<=len(m):
        v=u32(m,o)
        if v==0xFFFFFFFF or m[o:o+4] in (b'Data',b'Anim',b'AltN',b'HEnd'): break
        out.append(v); o+=4
    return out
def walk(m, node, st, seen, depth=0):
    if node==0 or node in seen or node+0x1c>len(m) or depth>64: return
    seen.add(node)
    t=u32(m,node)
    if t>>16: return
    nc=u32(m,node+0x14); cp=u32(m,node+0x18)
    if nc>4096 or cp+4*nc>len(m): return
    kids=[u32(m,cp+4*i) for i in range(nc)]
    if t==0x3064:
        for k in kids:
            if k==0 or k in seen or k+0x3c>len(m): continue
            seen.add(k)
            nprim=u16(m,k+0x20); ptype=u16(m,k+0x22); psz=u32(m,k+0x24)
            nv=u16(m,k+0x3a); vp=u32(m,k+0x34); dl=u32(m,k+0x30)
            if vp==0: continue  # collision-only mesh
            st['meshes']+=1; st['verts']+=nv
            tris=0
            if psz and ptype in (3,4,5):
                # primitive sizes array: vertices per primitive (strips/fans) or tri count
                for i in range(nprim):
                    s=u32(m,psz+4*i); tris+=max(s-2,0) if ptype==5 else 1
            elif ptype==3: tris=nprim
            elif ptype==4: tris=nprim*2
            st['tris_hdr']+=tris
            # count triangles from display list (F3DEX: 0x05 tri1/0x06 tri2/0x07 quad; F3DEX2: 0x05/0x06/0x07)
            if dl:
                o=dl; t1=0
                while o+8<=len(m):
                    op=m[o]
                    if op==0xDF or op==0xB8: break
                    if op==0x05: t1+=1
                    elif op in (0x06,0x07): t1+=2
                    o+=8
                st['tris_dl']+=t1
    else:
        for k in kids: walk(m,k,st,seen,depth+1)
def stats(m):
    st={'meshes':0,'verts':0,'tris_hdr':0,'tris_dl':0}
    seen=set()
    for r in roots(m): walk(m,r,st,seen)
    return st

def decomp(src):
    # port of func_80011940: LZSS, 4 KB ring window starting at index 1, flag bit 1 = literal
    win = bytearray(0x1000); pos = 1; i = 0; out = bytearray()
    while True:
        flags = src[i]; i += 1
        for b in range(8):
            if flags & (1 << b):
                c = src[i]; i += 1
                out.append(c); win[pos] = c; pos = (pos + 1) & 0xFFF
            else:
                b1, b2 = src[i], src[i+1]; i += 2
                off = ((b1 & 0xF) << 8) | b2
                if off == 0: return bytes(out)
                for k in range((b1 >> 4) + 2):
                    c = win[(off + k) & 0xFFF]
                    out.append(c); win[pos] = c; pos = (pos + 1) & 0xFFF

if __name__=='__main__':
    path,base=sys.argv[1],int(sys.argv[2],0)
    d,ms=load(path,base)
    for i,(s,e) in enumerate(ms):
        tag=d[s:s+4]
        m=d[s:e]
        if m[:4]==b'Comp': m=decomp(m[12:])
        print(i,m[:4].decode('latin1'),len(m),stats(m))

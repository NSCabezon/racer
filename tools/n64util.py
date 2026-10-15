#!/usr/bin/env python3
"""
n64util.py — ROM/symbol helpers used while identifying functions.

    python3 tools/n64util.py dis  <vram_hex> <size_hex>   # disassemble raw address range
    python3 tools/n64util.py xref <addr_hex> [<hi_hex>]   # lw/sw/la references to a global
    python3 tools/n64util.py fn   <vram_hex>              # which symbols.toml function covers it

Run from the repo root (reads baserom.z64 and symbols.toml).
"""
import struct,re,sys
ROM='baserom.z64'; BASE=0x80000400; ROMOFF=0x1000; END=0x80099000
rom=open(ROM,'rb').read()
syms=[]; cur=None
for line in open('symbols.toml'):
    m=re.match(r'\s*name = "([^"]+)"',line)
    if m: cur=[m.group(1),None,None]
    m=re.match(r'\s*vram = (0x[0-9A-Fa-f]+)',line)
    if m and cur: cur[1]=int(m.group(1),16)
    m=re.match(r'\s*size = (0x[0-9A-Fa-f]+)',line)
    if m and cur: cur[2]=int(m.group(1),16); syms.append(tuple(cur)); cur=None
syms=[s for s in syms if s[0]!="code"]; syms.sort(key=lambda s:s[1])
def fn(v):
    for n,a,s in syms:
        if a<=v<a+s: return n
    return "?"
def word(v): o=v-BASE+ROMOFF; return struct.unpack('>I',rom[o:o+4])[0]
sys.path.insert(0,'tools')
from func_differ import disassemble_instruction
def dis(v,n):
    for a in range(v,v+n,4):
        w=word(a); print(f"  {a:08X}: {w:08X}  {disassemble_instruction(w,a)}")
def xref(lo,hi):
    regs={}; hits={}
    for v in range(BASE,END,4):
        w=word(v); op=w>>26; rs=(w>>21)&31; rt=(w>>16)&31; imm=w&0xffff
        simm=imm-0x10000 if imm&0x8000 else imm
        if op==0x0f: regs[rt]=imm<<16; continue
        if op==0x09 and rs in regs:  # addiu rt, rs, imm (address formation)
            addr=(regs[rs]+simm)&0xffffffff
            if lo<=addr<=hi: hits.setdefault((fn(v),addr,'la'),[]).append(v)
            continue
        if 0x20<=op<=0x2e and rs in regs:
            addr=(regs[rs]+simm)&0xffffffff
            if lo<=addr<=hi:
                k={0x23:'lw',0x2b:'sw',0x25:'lhu',0x29:'sh',0x24:'lbu',0x28:'sb',0x21:'lh',0x20:'lb',0x2e:'swr',0x2a:'swl'}.get(op,'op%x'%op)
                hits.setdefault((fn(v),addr,k),[]).append(v)
    for (f,a,k),vs in sorted(hits.items(),key=lambda x:x[1][0]):
        print(f"  {f:36s} {k:4s} 0x{a:08X}  at {' '.join('%08X'%x for x in vs[:5])}")
if __name__=='__main__':
    cmd=sys.argv[1]
    if cmd=='dis': dis(int(sys.argv[2],16), int(sys.argv[3],16))
    elif cmd=='xref': xref(int(sys.argv[2],16), int(sys.argv[3],16) if len(sys.argv)>3 else int(sys.argv[2],16))
    elif cmd=='fn': print(fn(int(sys.argv[2],16)))

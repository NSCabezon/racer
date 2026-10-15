#!/usr/bin/env python3
"""
find_missing_funcs.py — find code addresses used as function pointers (lui/addiu
pairs in code, pointer tables in data) that are not the start of a symbols.toml
function but look like one (preceded by jr ra / prologue). These show up at
runtime as "Failed to find function at 0x...".

    python3 tools/find_missing_funcs.py          # report
    python3 tools/find_missing_funcs.py apply    # split symbols.toml; libultra-region ones are stubbed

Run from the repo root, then re-run N64Recomp + the post-recomp scripts.
"""
import os
import sys,struct,re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import n64util as N
starts={a for n,a,s in N.syms}
CODE_LO,CODE_HI=N.BASE,N.END
def is_jr_ra(w): return w==0x03E00008
def plausible_start(a):
    # previous instr is delay slot of jr ra, or nop padding after jr ra
    p1=N.word(a-4); p2=N.word(a-8)
    if is_jr_ra(p2): return True
    if p1==0 and (is_jr_ra(N.word(a-12)) or N.word(a-8)==0): return True
    w=N.word(a)
    if (w>>16)==0x27BD and (w&0x8000): return True  # addiu sp,sp,-N
    return False
cands={}
# 1) lui/addiu address formation in code
regs={}
for v in range(CODE_LO,CODE_HI,4):
    w=N.word(v); op=w>>26; rs=(w>>21)&31; rt=(w>>16)&31; imm=w&0xffff
    simm=imm-0x10000 if imm&0x8000 else imm
    if op==0x0f: regs[rt]=imm<<16; continue
    if op==0x09 and rs in regs:
        a=(regs[rs]+simm)&0xffffffff
        if CODE_LO<=a<CODE_HI and a%4==0 and a not in starts: cands.setdefault(a,set()).add('la@%08X'%v)
# 2) pointer tables in data (rest of ROM up to 0x100000 after code)
data_lo=CODE_HI; data_hi=0x800A7000
for v in range(data_lo,data_hi,4):
    w=N.word(v)
    if CODE_LO<=w<CODE_HI and w%4==0 and w not in starts: cands.setdefault(w,set()).add('data@%08X'%v)
good=[]; dubious=[]
for a in sorted(cands):
    (good if plausible_start(a) else dubious).append(a)
print("plausible missing function starts:",len(good)); 
for a in good: print("  %08X in %-24s refs=%s"%(a,N.fn(a),','.join(sorted(cands[a])[:3])))
print("dubious (not split):",len(dubious))
for a in dubious[:40]: print("  %08X in %-24s refs=%s"%(a,N.fn(a),','.join(sorted(cands[a])[:2])))
if len(sys.argv)>1 and sys.argv[1]=='apply':
    s=open('symbols.toml').read(); n=0; stubbed=[]
    for a in good:
        f=N.fn(a)
        if f=='?' or a==0x800980C0: continue
        if a>=0x8008A000: stubbed.append('func_%08X'%a)
        m=re.search(r'(  \[\[section\.functions\]\]\n(?:  #[^\n]*\n)*  name = "%s"\n  vram = (0x[0-9A-Fa-f]+)\n  size = (0x[0-9A-Fa-f]+)\n)'%re.escape(f), s)
        if not m: print("  !! could not patch",f); continue
        start=int(m.group(2),16); size=int(m.group(3),16)
        if not (start<a<start+size): print("  !! range mismatch",f); continue
        new=m.group(1).replace("size = %s"%m.group(3),"size = 0x%X"%(a-start))
        new+='\n  [[section.functions]]\n  # Auto-split: referenced as a function pointer; starts with a prologue after jr ra.\n  name = "func_%08X"\n  vram = 0x%08X\n  size = 0x%X\n'%(a,a,start+size-a)
        s=s.replace(m.group(1),new,1); n+=1
        # refresh symbol table so later splits inside the same function resolve
        N.syms.append(("func_%08X"%a,a,start+size-a)); N.syms=[x if x[0]!=f else (f,start,a-start) for x in N.syms]; N.syms.sort(key=lambda x:x[1])
    open('symbols.toml','w').write(s); print("applied splits:",n)
    r=open('recomp.toml').read()
    block='\n'.join('    "%s",'%x for x in stubbed)
    r=r.replace('    "libultra_split_8008D12C",','    # Auto-split thread entry points / handlers in the libultra region (hardware code, stubbed)\n'+block+'\n    "libultra_split_8008D12C",',1)
    open('recomp.toml','w').write(r); print('stubbed:',stubbed)

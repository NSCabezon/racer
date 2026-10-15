#!/usr/bin/env python3
"""
post_recomp_patches.py — re-apply hand-written redirects that can't be expressed
in symbols.toml (i.e. routing a recompiled game function to a different
ultramodern *_recomp reimplementation).

Run after every N64Recomp regeneration, together with fix_fallthroughs.py:
    N64Recomp.exe recomp.toml
    py tools/fix_fallthroughs.py
    py tools/post_recomp_patches.py

Each entry redirects a function's body to call <target>(rdram, ctx) and return,
discarding the recompiled body. Idempotent.

Why these can't be symbols.toml renames:
- func_80087D70 is the game's osPiStartDma, but the name "osPiStartDma" is
  already taken by another copy (0x80095820). Routing it to ultramodern's
  osPiStartDma_recomp bypasses the stubbed PI-manager thread so DMAs actually
  complete (otherwise the game blocks forever on the DMA reply queue).
"""
import re, glob, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RECOMP_DIR = os.path.join(ROOT, "RecompiledFuncs")
MARKER = "@post-recomp-redirect"

# func name -> ultramodern reimplementation to call instead
REDIRECTS = {
    "func_80087D70": "swe1r_osPiStartDma_recomp",   # game osPiStartDma -> logged wrapper over librecomp's synchronous DMA (bypasses the dead PI manager)
    # osPiReadIo(devAddr, u32* data): the recompiled body spins on PI_STATUS and
    # reads the cart through 0xB0000000 (unmapped). librecomp has no osPiReadIo
    # wrapper (only osEPiReadIo, which takes a handle), so the host implements it.
    "osPiReadIo_8008A710": "swe1r_osPiReadIo_recomp",
}

# Functions N64Recomp renames to *_recomp (ignored_funcs list) but never
# declares in funcs.h. MSVC accepted the implicit declaration; clang/gcc do
# not. Each is implemented host-side (src/main.cpp) with the recomp signature.
EXTRA_DECLS = [
    "__osSiRawStartDma_recomp",
    "swe1r_osSetEventMesg_recomp",
    "swe1r_osPiReadIo_recomp",
    "swe1r_osViBlack_recomp",
    "swe1r_osPiStartDma_recomp",
    "swe1r_osSpTaskYield_recomp",
    "swe1r_osSpTaskYielded_recomp",
    "swe1r_osViSetSpecialFeatures_recomp",
]

# Call-site rewrites: callee name -> replacement, applied to every call in the
# generated funcs_*.c. Used when the callee is a runtime reimplementation we
# cannot override at link time (defining it again would clash with librecomp)
# but whose behaviour the game needs extended.
#
# - osSetEventMesg: ultramodern's version only records SP/DP/AI/SI. The game
#   registers its VI retrace queue through OS_EVENT_VI here as well, and the
#   runtime's VI thread only delivers to the queue set via osViSetEvent, so
#   the scheduler would block forever in osRecvMesg. swe1r_osSetEventMesg_recomp
#   (src/main.cpp) forwards VI to osViSetEvent and passes the rest through.
# - osViBlack: the game only ever calls osViBlack(1) (hardware clears the flag
#   on the next VI swap); ultramodern keeps it until osViBlack(0). The host
#   wrapper applies one-shot semantics.
# - osSpTaskYield / osSpTaskYielded: librecomp ignores yields and always answers
#   "not yielded". This game's SP scheduler requests a yield whenever an audio
#   task is in flight at frame start and only clears its "audio pending" flag
#   when the task reports yielded; with instant audio tasks that never happens
#   and the next frame waits forever. The host wrappers answer "yielded" after
#   a yield request so the game resumes the task later instead of deadlocking.
CALL_REWRITES = {
    "osSetEventMesg_recomp": "swe1r_osSetEventMesg_recomp",
    "osViBlack_recomp": "swe1r_osViBlack_recomp",
    "osSpTaskYield_recomp": "swe1r_osSpTaskYield_recomp",
    "osSpTaskYielded_recomp": "swe1r_osSpTaskYielded_recomp",
    "osViSetSpecialFeatures_recomp": "swe1r_osViSetSpecialFeatures_recomp",
}

def add_missing_decls():
    fp = os.path.join(RECOMP_DIR, "funcs.h")
    txt = open(fp, encoding="utf-8").read()
    added = 0
    for name in EXTRA_DECLS:
        decl = f"void {name}(uint8_t* rdram, recomp_context* ctx);"
        if decl not in txt:
            block = f"// {MARKER}: host-implemented, missing from generated header\n{decl}\n"
            # Insert before the trailing `#ifdef __cplusplus } #endif` so the
            # declaration is visible to both C and C++ translation units.
            idx = txt.rfind("#ifdef __cplusplus")
            txt = (txt[:idx] + block + txt[idx:]) if idx != -1 else txt.rstrip("\n") + "\n" + block
            added += 1
    if added:
        open(fp, "w", encoding="utf-8").write(txt)
    print(f"post-recomp declarations added: {added}")

def rewrite_calls():
    total = 0
    for fp in glob.glob(os.path.join(RECOMP_DIR, "funcs_*.c")):
        txt = open(fp, encoding="utf-8", errors="replace").read()
        new = txt
        for src, dst in CALL_REWRITES.items():
            new, n = re.subn(r'\b' + re.escape(src) + r'\(rdram, ctx\);', dst + '(rdram, ctx);', new)
            total += n
        if new != txt:
            open(fp, "w", encoding="utf-8").write(new)
    print(f"post-recomp call rewrites applied: {total}")

def main():
    add_missing_decls()
    rewrite_calls()
    patched = 0
    for fp in glob.glob(os.path.join(RECOMP_DIR, "funcs_*.c")):
        lines = open(fp, encoding="utf-8", errors="replace").read().split("\n")
        out = []
        i = 0
        changed = False
        while i < len(lines):
            out.append(lines[i])
            m = re.match(r'^RECOMP_FUNC\s+void\s+(\w+)\s*\(', lines[i])
            if m and m.group(1) in REDIRECTS:
                name = m.group(1); target = REDIRECTS[name]
                # copy the two prologue decl lines, then inject the redirect
                # (only if not already injected)
                lookahead = "\n".join(lines[i:i+8])
                if MARKER in lookahead:
                    i += 1; continue
                # emit decls (next 2 non-empty lines are the hi/lo + c1cs decls)
                j = i + 1
                while j < len(lines) and lines[j].strip() in (
                        "uint64_t hi = 0, lo = 0, result = 0;", "int c1cs = 0;"):
                    out.append(lines[j]); j += 1
                out.append(f"    // {MARKER}: route to {target}")
                out.append(f"    {target}(rdram, ctx);")
                out.append("    return;")
                i = j; changed = True; patched += 1
                continue
            i += 1
        if changed:
            open(fp, "w", encoding="utf-8").write("\n".join(out))
    print(f"post-recomp redirects applied: {patched}")

if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
make_texture_pack.py — build an RT64 texture replacement pack.

Input: a folder of replacement images named like RT64's dumps,
<hash16>.v<N>.png (or .dds), optionally in subfolders. Output: the same
folder with an rt64.json, or a .rtz (zip) ready for texture_packs/.

    python3 tools/make_texture_pack.py SRC_DIR texture_packs/hd.rtz
    python3 tools/make_texture_pack.py SRC_DIR            # writes SRC_DIR/rt64.json

Dump the originals with SWE1R_DUMP_TEXTURES=dir, edit or upscale the PNGs you
want to replace (keep the file names), copy only those into SRC_DIR, then run
this. The game loads every *.rtz or folder with rt64.json in texture_packs/
(or SWE1R_TEXTURE_PACKS) at startup.
"""
import json, os, re, sys, zipfile

NAME = re.compile(r"^([0-9a-fA-F]{16})\.v(\d+)\.(png|dds)$", re.IGNORECASE)


def collect(src):
    textures, versions = [], set()
    for dirpath, _, files in os.walk(src):
        for f in sorted(files):
            m = NAME.match(f)
            if not m:
                continue
            rel = os.path.relpath(os.path.join(dirpath, f), src).replace(os.sep, "/")
            textures.append({"path": rel, "hashes": {"rt64": m.group(1).lower()}})
            versions.add(int(m.group(2)))
    return textures, versions


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    textures, versions = collect(src)
    if not textures:
        print(f"no <hash>.v<N>.png/.dds files found in {src}")
        return 1
    if len(versions) > 1:
        print(f"warning: mixed hash versions {sorted(versions)}; using the highest")
    db = {
        "configuration": {"autoPath": "rt64", "configurationVersion": 3, "hashVersion": max(versions)},
        "textures": textures,
    }
    text = json.dumps(db, indent=2)
    if out is None or not out.endswith(".rtz"):
        dst = out or src
        os.makedirs(dst, exist_ok=True)
        with open(os.path.join(dst, "rt64.json"), "w") as fp:
            fp.write(text)
        print(f"wrote {os.path.join(dst, 'rt64.json')} ({len(textures)} textures)")
        return 0
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
        z.writestr("rt64.json", text)
        for t in textures:
            z.write(os.path.join(src, t["path"]), t["path"])
    print(f"wrote {out} ({len(textures)} textures)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

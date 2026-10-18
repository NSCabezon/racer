#!/bin/bash
# Build the HD texture pack from the textures dumped so far.
#
#   SWE1R_DUMP_TEXTURES=tex_dump ./build/SWE1Racer   # play what you want covered
#   tools/build_hd_pack.sh                           # -> texture_packs/hd.rtz
#
# Incremental: only textures not yet in hd_out/ are upscaled. Hashes listed in
# textures/exclude.txt are neither upscaled nor packed (the game keeps the
# original). Nothing generated here is versioned; dumps, upscales and packs
# are all gitignored.
set -euo pipefail
cd "$(dirname "$0")/.."

SRC=${SRC:-tex_dump}
HD=${HD:-hd_out}
PACK=${PACK:-texture_packs/hd.rtz}
EXCLUDE=textures/exclude.txt

python3 tools/upscale_textures.py "$SRC" "$HD" --skip-existing --exclude "$EXCLUDE"
# font atlases: vector redraw instead of AI upscaling (always rewritten)
python3 tools/font_atlas_hd.py "$SRC" "$HD"

STAGE=$(mktemp -d); trap 'rm -rf "$STAGE"' EXIT
python3 - "$HD" "$STAGE" "$EXCLUDE" <<'PY'
import os, sys
hd, stage, excl = sys.argv[1:]
skip = {l.strip().split(".")[0].lower() for l in open(excl)
        if l.strip() and not l.startswith("#")}
n = 0
for f in os.listdir(hd):
    if f.lower().endswith(".png") and f.split(".")[0].lower() not in skip:
        os.symlink(os.path.abspath(os.path.join(hd, f)), os.path.join(stage, f))
        n += 1
print(f"packing {n} textures ({len(skip)} excluded)")
PY

mkdir -p "$(dirname "$PACK")"
python3 tools/make_texture_pack.py "$STAGE" "$PACK"

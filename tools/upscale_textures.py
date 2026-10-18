#!/usr/bin/env python3
"""
upscale_textures.py — AI-upscale RT64 texture dumps for a replacement pack.

    python3 tools/upscale_textures.py SRC_DIR DST_DIR [--model realesrgan-x4plus]
        [--bin PATH] [--min-size 8] [--only LIST.txt] [--exclude LIST.txt]
        [--skip-existing]

SRC_DIR holds dumped textures (<hash>.v<N>.png, from SWE1R_DUMP_TEXTURES).
DST_DIR receives 4x versions with the same names, ready for
tools/make_texture_pack.py.

Rules:
- Seams: each texture is padded to 3x3 before upscaling and the centre is
  cropped back. The padding follows the texture's S/T address modes from the
  dump's .tile.json: wrap repeats it, mirror flips it, clamp stretches the edge
  pixels. Wrapping a clamped texture (menu backgrounds built from 64x32
  pieces) bleeds its opposite edge in and shows a grid of seams in game.
- Mosaics: big 2D images (title background, boot logos) are drawn as many
  small clamped tiles loaded back to back in RDRAM (.rice.json). Runs of >= 4
  clamped tiles of one height and format, each starting where the previous
  one ends, are laid out by trying column counts and S/T flips and scoring
  how well neighbouring edges match; if the best layout clearly beats random
  pairs, the whole image is upscaled at once and cut back into tiles, so the
  AI sees across tile borders. Otherwise the tiles stay single. Tile widths
  may vary when they repeat row by row (Nintendo logo: 64+64+24; the last
  tile of a row is narrower); a run can also be a single row (title words).
  --plan only prints the detected mosaics.
- Alpha: upscaled separately as a grey image. If the original alpha is only
  0/255 (cut-outs: foliage, fences, text) the result is thresholded back to
  0/255 to keep edges crisp.
- Textures smaller than --min-size on either side are skipped (they are
  usually gradients or masks that gain nothing).
- --only / --exclude take files with one hash or file name per line.

Needs realesrgan-ncnn-vulkan (https://github.com/xinntao/Real-ESRGAN, release
v0.2.5.0 macOS zip). Default location: ../tools_ext/realesrgan/ next to the
repo, or set REALESRGAN_BIN.
"""
import argparse, json, os, random, shutil, subprocess, sys, tempfile
from PIL import Image, ImageChops, ImageStat

SCALE = 4
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BIN = os.environ.get(
    "REALESRGAN_BIN",
    os.path.join(os.path.dirname(REPO), "tools_ext", "realesrgan", "realesrgan-ncnn-vulkan"),
)


def read_list(path):
    if not path:
        return None
    with open(path) as fp:
        return {line.strip().split(".")[0].lower() for line in fp if line.strip() and not line.startswith("#")}


WRAP, MIRROR, CLAMP = "wrap", "mirror", "clamp"


def address_modes(src, name):
    """(S, T) address modes from RT64's <hash>.v<N>.tile.json next to the dump.
    G_TX_CLAMP (2) wins over G_TX_MIRROR (1); neither means wrap."""
    path = os.path.join(src, name[: -len(".png")] + ".tile.json")
    try:
        with open(path) as fp:
            tile = json.load(fp)["tile"]
    except (OSError, ValueError, KeyError):
        return WRAP, WRAP
    mode = lambda cm: CLAMP if cm & 2 else MIRROR if cm & 1 else WRAP
    return mode(tile["cms"]), mode(tile["cmt"])


def pad_x(im, mode):
    """Surround horizontally with what the sampler would see past each edge."""
    w, h = im.size
    if mode == WRAP:
        left = right = im
    elif mode == MIRROR:
        left = right = im.transpose(Image.FLIP_LEFT_RIGHT)
    else:
        left = im.crop((0, 0, 1, h)).resize((w, h), Image.NEAREST)
        right = im.crop((w - 1, 0, w, h)).resize((w, h), Image.NEAREST)
    big = Image.new(im.mode, (w * 3, h))
    for i, part in enumerate((left, im, right)):
        big.paste(part, (i * w, 0))
    return big


def pad3(im, modes):
    """3x3 context around the texture following its S/T address modes, so the
    upscaled edges continue the way the texture is sampled in game (repeating,
    mirrored or held at the edge) instead of always wrapping."""
    rot = lambda m: m.transpose(Image.TRANSPOSE)
    return rot(pad_x(rot(pad_x(im, modes[0])), modes[1]))


BYTES_PER_TEXEL = {0: 0.5, 1: 1, 2: 2, 3: 4}  # by G_IM_SIZ
MIN_RUN, MAX_COLS, ACCEPT, FLAT_STDDEV = 4, 32, 0.6, 4


def tile_info(src, name):
    """(width, height, siz, fmt, rdram address) from the dump's .tile/.rice.json."""
    base = os.path.join(src, name[: -len(".png")])
    try:
        with open(base + ".tile.json") as fp:
            tile = json.load(fp)
        with open(base + ".rice.json") as fp:
            addr = json.load(fp)["texture"]["address"]
        return tile["width"], tile["height"], tile["tile"]["siz"], tile["tile"]["fmt"], addr
    except (OSError, ValueError, KeyError):
        return None


def find_runs(src, names):
    """Runs of clamped textures of one height and format, each loaded right
    after the previous one (addr + its own size in bytes), so widths may vary.
    Following by address lets textures of other screens interleave in the same
    heap without breaking a run. Addresses seen twice break the run
    (ambiguous). Returns [[(addr, name, width), ...], ...]."""
    groups = {}
    for name in names:
        if address_modes(src, name) != (CLAMP, CLAMP):
            continue
        info = tile_info(src, name)
        if info is None or info[2] not in BYTES_PER_TEXEL:
            continue
        w, h, siz, fmt, addr = info
        groups.setdefault((h, siz, fmt), {}).setdefault(addr, []).append((name, w))
    runs = []
    for (h, siz, fmt), by_addr in groups.items():
        unique = {addr: ns[0] for addr, ns in by_addr.items() if len(ns) == 1}
        nxt = lambda addr: addr + int(unique[addr][1] * h * BYTES_PER_TEXEL[siz])
        starts = set(unique) - {nxt(addr) for addr in unique}
        for addr in sorted(starts):
            run = []
            while addr in unique:
                run.append((addr, unique[addr][0], unique[addr][1]))
                addr = nxt(addr)
            if len(run) >= MIN_RUN:
                runs.append(run)
    return sorted(runs)


def row_period(widths):
    """Smallest p >= 2 such that widths repeat every p tiles, None if none."""
    n = len(widths)
    for p in range(2, n):
        if all(widths[i] == widths[i % p] for i in range(n)):
            return p
    return None


def orient(im, flip_s, flip_t):
    """Flip a tile; the same call undoes it."""
    if flip_s:
        im = im.transpose(Image.FLIP_LEFT_RIGHT)
    if flip_t:
        im = im.transpose(Image.FLIP_TOP_BOTTOM)
    return im


def edges(im):
    """(crop, flat) for the L, R, T, B one-pixel edges of an RGBA tile. Flat
    edges (transparent or one colour) match anything, so they are no evidence."""
    w, h = im.size
    out = []
    for box in ((0, 0, 1, h), (w - 1, 0, w, h), (0, 0, w, 1), (0, h - 1, w, h)):
        e = im.crop(box)
        st = ImageStat.Stat(e)
        out.append((e, st.extrema[3][1] == 0 or max(st.stddev[:3]) < FLAT_STDDEV))
    return out


def seam(a, b):
    """Mean RGBA difference between two edges, None if both are flat."""
    if a[1] and b[1]:
        return None
    return sum(ImageStat.Stat(ImageChops.difference(a[0], b[0])).mean) / 4


def mean_seams(values, total):
    """Mean of the informative seams, None if too few of them to judge."""
    vals = [v for v in values if v is not None]
    if len(vals) < max(2, total // 4):
        return None
    return sum(vals) / len(vals)


def detect_layout(tiles):
    """Best (cols, flip_s, flip_t, h_err, v_err, h_base, v_base) for a run of
    RGBA tiles in address order, or None if no layout clearly fits. Tiles go
    row-major; errors are mean edge differences between neighbours, baselines
    the same over random non-neighbour pairs. Same-width tiles try every
    column count; mixed widths only the period they repeat with. A single row
    (cols == n) is judged on its horizontal seams alone (v_err is None) and
    only tried when no layout with several rows fits, since one row always
    has fewer seams to get wrong."""
    n = len(tiles)
    widths = [t.size[0] for t in tiles]
    if len(set(widths)) == 1:
        candidates = [c for c in range(2, min(MAX_COLS, n - 1) + 1)]
    else:
        p = row_period(widths)
        candidates = [p] if p else []
    lay = _best_layout(tiles, widths, candidates)
    if lay is None and n <= MAX_COLS and widths[0] == max(widths):
        lay = _best_layout(tiles, widths, [n])  # narrower tiles only at the row's end
    return lay


def trim_tail(tiles, cols, flip_s, flip_t, h_base, v_base):
    """How many leading tiles of a laid-out run to keep: runs often continue
    into an unrelated texture loaded right after the image, so cut at the
    first tile of the tail (last row, or second half of a single row) whose seam to its neighbour (above, or to
    the left in a single row) is as bad as a random pair."""
    e = [edges(orient(t, flip_s, flip_t)) for t in tiles]
    n = len(tiles)
    # only the tail can be foreign: the second half of a single row, or the last row
    first = n // 2 if cols >= n else (n - 1) // cols * cols
    for k in range(max(first, 1), n):
        if cols >= n:
            bad = k and (seam(e[k - 1][1], e[k][0]) or 0) >= h_base
        else:
            bad = k >= cols and (seam(e[k - cols][3], e[k][2]) or 0) >= v_base
        if bad:
            return k
    return n


def _best_layout(tiles, widths, candidates):
    n = len(tiles)
    plain = [edges(t) for t in tiles]
    rng = random.Random(0)
    pairs = [(i, j) for i, j in ((rng.randrange(n), rng.randrange(n)) for _ in range(400))
             if abs(i - j) > 1][:200]
    h_base = mean_seams([seam(plain[i][1], plain[j][0]) for i, j in pairs], len(pairs))
    vpairs = [(i, j) for i, j in pairs if widths[i] == widths[j]]
    v_base = mean_seams([seam(plain[i][3], plain[j][2]) for i, j in vpairs], len(vpairs))
    if h_base is None or h_base < 1:
        return None  # flat tiles: every edge matches every other
    best = None
    for flip_s in (False, True):
        for flip_t in (False, True):
            e = [edges(orient(t, flip_s, flip_t)) for t in tiles]
            hd = [seam(e[i][1], e[i + 1][0]) for i in range(n - 1)]
            for cols in candidates:
                hp = [hd[i] for i in range(n - 1) if i % cols != cols - 1]
                h_err = mean_seams(hp, len(hp))
                if h_err is None:
                    continue
                if cols >= n:
                    v_err, score = None, 2 * h_err / h_base
                else:
                    if v_base is None or v_base < 1:
                        continue
                    vp = [seam(e[i][3], e[i + cols][2]) for i in range(n - cols)]
                    v_err = mean_seams(vp, len(vp))
                    if v_err is None:
                        continue
                    score = h_err / h_base + v_err / v_base
                if best is None or score < best[0]:
                    best = (score, cols, flip_s, flip_t, h_err, v_err)
    if best is None or best[4] >= ACCEPT * h_base:
        return None
    if best[5] is not None and best[5] >= ACCEPT * v_base:
        return None
    return best[1:] + (h_base, v_base)


def cell_offsets(widths, cols):
    """x offset of each column (columns share the width of their first row)."""
    xs, x = [], 0
    for c in range(cols):
        xs.append(x)
        x += widths[c]
    return xs, x


def build_mosaic(tiles, cols, flip_s, flip_t):
    """Paste oriented RGBA tiles row-major. Missing cells of a partial last row
    hold the bottom edge of the tile above, stretched (cropped away later)."""
    widths = [t.size[0] for t in tiles]
    h = tiles[0].size[1]
    xs, total_w = cell_offsets(widths, cols)
    rows = (len(tiles) + cols - 1) // cols
    big = Image.new("RGBA", (total_w, rows * h))
    oriented = [orient(t, flip_s, flip_t) for t in tiles]
    for i in range(rows * cols):
        x, y, w = xs[i % cols], (i // cols) * h, widths[i % cols]
        if i < len(tiles):
            big.paste(oriented[i], (x, y))
        else:
            big.paste(big.crop((x, y - 1, x + w, y)).resize((w, h), Image.NEAREST), (x, y))
    return big


def run_model(binary, model, src, dst):
    cmd = [binary, "-i", src, "-o", dst, "-n", model, "-s", str(SCALE), "-f", "png"]
    r = subprocess.run(cmd, cwd=os.path.dirname(binary), capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"realesrgan failed ({r.returncode})")


def alpha_kind(al):
    """opaque / binary (only 0 and 255) / smooth."""
    if al.getextrema()[0] == 255:
        return "opaque"
    return "binary" if not any(al.histogram()[1:255]) else "smooth"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--model", default="realesrgan-x4plus")
    ap.add_argument("--bin", default=DEFAULT_BIN)
    ap.add_argument("--min-size", type=int, default=8)
    ap.add_argument("--only")
    ap.add_argument("--exclude")
    ap.add_argument("--skip-existing", action="store_true",
                    help="leave textures already in DST_DIR alone (incremental runs)")
    ap.add_argument("--plan", action="store_true", help=argparse.SUPPRESS)  # print mosaics only
    a = ap.parse_args()

    if not a.plan and not os.path.isfile(a.bin):
        raise SystemExit(f"upscaler not found at {a.bin} (set --bin or REALESRGAN_BIN)")
    only, exclude = read_list(a.only), read_list(a.exclude)

    names, images = [], {}
    for name in sorted(os.listdir(a.src)):
        if not name.lower().endswith(".png"):
            continue
        key = name.split(".")[0].lower()
        if (only is not None and key not in only) or (exclude and key in exclude):
            continue
        im = Image.open(os.path.join(a.src, name)).convert("RGBA")
        if im.size[0] < a.min_size or im.size[1] < a.min_size:
            continue
        names.append(name)
        images[name] = im

    # Mosaics first; whatever is not claimed by one is upscaled on its own.
    mosaics = []  # (addr, names, cols, flip_s, flip_t)
    def candidates(runs):
        """Each run, then (if it mixes widths) its same-width stretches, which
        are tried only when the whole run finds no layout."""
        for run in runs:
            yield run, False
            if len({w for _, _, w in run}) > 1:
                start = 0
                for i in range(1, len(run) + 1):
                    if i == len(run) or run[i][2] != run[start][2]:
                        if i - start >= MIN_RUN:
                            yield run[start:i], True
                        start = i

    placed = set()
    for run, is_part in candidates(find_runs(a.src, names)):
        run_names = [n for _, n, _ in run]
        if is_part and placed & set(run_names):
            continue  # the whole run already became a mosaic
        lay = detect_layout([images[n] for n in run_names])
        ws = sorted({images[n].size[0] for n in run_names}, reverse=True)
        size = f"{'/'.join(map(str, ws))}x{images[run_names[0]].size[1]}"
        if lay is None:
            print(f"run 0x{run[0][0]:x}: {len(run)} tiles {size}, no layout fits, kept single")
            continue
        cols, flip_s, flip_t, h_err, v_err, h_base, v_base = lay
        keep = trim_tail([images[n] for n in run_names], cols, flip_s, flip_t, h_base, v_base)
        if keep < MIN_RUN:
            print(f"run 0x{run[0][0]:x}: {len(run)} tiles {size}, layout breaks after {keep}, kept single")
            continue
        if keep < len(run_names):
            print(f"run 0x{run[0][0]:x}: last {len(run_names) - keep} tiles don't continue the image, left single")
            run_names = run_names[:keep]
        cols = min(cols, len(run_names))
        flips = "".join(f for f, on in (("flipS", flip_s), ("flipT", flip_t)) if on)
        v = f"v {v_err:.1f}/{v_base:.1f}" if v_err is not None else "one row"
        print(f"mosaic 0x{run[0][0]:x}: {len(run_names)} tiles {size}, {cols} cols"
              + (f", {flips}" if flips else "")
              + f"  (seam h {h_err:.1f}/{h_base:.1f} {v})")
        mosaics.append((run[0][0], run_names, cols, flip_s, flip_t))
        placed |= set(run_names)
    claimed = {n for m in mosaics for n in m[1]}
    print(f"{len(mosaics)} mosaics ({len(claimed)} tiles), "
          f"{len(names) - len(claimed)} single textures")
    if a.plan:
        return

    os.makedirs(a.dst, exist_ok=True)
    work = tempfile.mkdtemp(prefix="upscale_")
    rgb_in, rgb_out = os.path.join(work, "rgb_in"), os.path.join(work, "rgb_out")
    al_in, al_out = os.path.join(work, "al_in"), os.path.join(work, "al_out")
    for d in (rgb_in, rgb_out, al_in, al_out):
        os.makedirs(d)

    exists = lambda n: a.skip_existing and os.path.exists(os.path.join(a.dst, n))
    # Each job is one image fed to the model, cut back into pieces:
    # (file, image, modes, [(name, x, y, flip_s, flip_t, kind)])
    jobs = []
    for addr, run_names, cols, flip_s, flip_t in mosaics:
        if all(exists(n) for n in run_names):
            continue
        tiles = [images[n] for n in run_names]
        h = tiles[0].size[1]
        xs, _ = cell_offsets([t.size[0] for t in tiles], cols)
        pieces = [(n, xs[k % cols], (k // cols) * h, flip_s, flip_t, alpha_kind(t.split()[3]))
                  for k, (n, t) in enumerate(zip(run_names, tiles))]
        jobs.append((f"mosaic_{addr:08x}.png", build_mosaic(tiles, cols, flip_s, flip_t),
                     (CLAMP, CLAMP), pieces))
    for name in names:
        if name in claimed or exists(name):
            continue
        im = images[name]
        jobs.append((name, im, address_modes(a.src, name),
                     [(name, 0, 0, False, False, alpha_kind(im.split()[3]))]))

    if not jobs:
        shutil.rmtree(work, ignore_errors=True)
        print("nothing to upscale")
        return
    for file, im, modes, pieces in jobs:
        r, g, b, al = im.split()
        pad3(Image.merge("RGB", (r, g, b)), modes).save(os.path.join(rgb_in, file))
        if any(p[5] != "opaque" for p in pieces):
            pad3(Image.merge("RGB", (al, al, al)), modes).save(os.path.join(al_in, file))

    print(f"upscaling {len(jobs)} images with {a.model}...")
    run_model(a.bin, a.model, rgb_in, rgb_out)
    if os.listdir(al_in):
        run_model(a.bin, a.model, al_in, al_out)

    written = 0
    for file, im, modes, pieces in jobs:
        W, H = im.size
        rgb_big = Image.open(os.path.join(rgb_out, file)).convert("RGB")
        al_big = None
        if os.path.exists(os.path.join(al_in, file)):
            al_big = Image.open(os.path.join(al_out, file)).convert("L")
        for name, x, y, flip_s, flip_t, kind in pieces:
            w, h = images[name].size
            x0, y0 = (W + x) * SCALE, (H + y) * SCALE
            box = (x0, y0, x0 + w * SCALE, y0 + h * SCALE)
            rgb = rgb_big.crop(box)
            if kind == "opaque":
                alpha = Image.new("L", rgb.size, 255)
            else:
                alpha = al_big.crop(box)
                if kind == "binary":
                    alpha = alpha.point(lambda v: 255 if v >= 128 else 0)
            out = orient(Image.merge("RGBA", (*rgb.split(), alpha)), flip_s, flip_t)
            out.save(os.path.join(a.dst, name))
            written += 1

    shutil.rmtree(work, ignore_errors=True)
    print(f"wrote {written} textures to {a.dst}")


if __name__ == "__main__":
    main()

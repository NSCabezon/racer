#!/usr/bin/env python3
"""
upscale_textures.py — AI-upscale RT64 texture dumps for a replacement pack.

    python3 tools/upscale_textures.py SRC_DIR DST_DIR [--model realesrgan-x4plus]
        [--bin PATH] [--min-size 8] [--only LIST.txt] [--exclude LIST.txt]

SRC_DIR holds dumped textures (<hash>.v<N>.png, from SWE1R_DUMP_TEXTURES).
DST_DIR receives 4x versions with the same names, ready for
tools/make_texture_pack.py.

Rules:
- Seams: each texture is tiled 3x3 (wrap) before upscaling and the centre is
  cropped back, so repeating textures still tile without a visible edge.
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
import argparse, os, shutil, subprocess, sys, tempfile
from PIL import Image

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


def tile3(im):
    w, h = im.size
    big = Image.new(im.mode, (w * 3, h * 3))
    for y in range(3):
        for x in range(3):
            big.paste(im, (x * w, y * h))
    return big


def run_model(binary, model, src, dst):
    cmd = [binary, "-i", src, "-o", dst, "-n", model, "-s", str(SCALE), "-f", "png"]
    r = subprocess.run(cmd, cwd=os.path.dirname(binary), capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"realesrgan failed ({r.returncode})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--model", default="realesrgan-x4plus")
    ap.add_argument("--bin", default=DEFAULT_BIN)
    ap.add_argument("--min-size", type=int, default=8)
    ap.add_argument("--only")
    ap.add_argument("--exclude")
    a = ap.parse_args()

    if not os.path.isfile(a.bin):
        raise SystemExit(f"upscaler not found at {a.bin} (set --bin or REALESRGAN_BIN)")
    only, exclude = read_list(a.only), read_list(a.exclude)
    os.makedirs(a.dst, exist_ok=True)

    work = tempfile.mkdtemp(prefix="upscale_")
    rgb_in, rgb_out = os.path.join(work, "rgb_in"), os.path.join(work, "rgb_out")
    al_in, al_out = os.path.join(work, "al_in"), os.path.join(work, "al_out")
    for d in (rgb_in, rgb_out, al_in, al_out):
        os.makedirs(d)

    jobs = []  # (name, w, h, alpha_kind)
    for name in sorted(os.listdir(a.src)):
        if not name.lower().endswith(".png"):
            continue
        key = name.split(".")[0].lower()
        if (only is not None and key not in only) or (exclude and key in exclude):
            continue
        im = Image.open(os.path.join(a.src, name)).convert("RGBA")
        w, h = im.size
        if w < a.min_size or h < a.min_size:
            continue
        r, g, b, al = im.split()
        tile3(Image.merge("RGB", (r, g, b))).save(os.path.join(rgb_in, name))
        lo, hi = al.getextrema()
        if lo == 255:
            kind = "opaque"
        else:
            values = set(al.getdata())
            kind = "binary" if values <= {0, 255} else "smooth"
            tile3(Image.merge("RGB", (al, al, al))).save(os.path.join(al_in, name))
        jobs.append((name, w, h, kind))

    if not jobs:
        raise SystemExit("nothing to upscale")
    print(f"upscaling {len(jobs)} textures with {a.model}...")
    run_model(a.bin, a.model, rgb_in, rgb_out)
    if os.listdir(al_in):
        run_model(a.bin, a.model, al_in, al_out)

    for name, w, h, kind in jobs:
        box = (w * SCALE, h * SCALE, 2 * w * SCALE, 2 * h * SCALE)
        rgb = Image.open(os.path.join(rgb_out, name)).convert("RGB").crop(box)
        if kind == "opaque":
            alpha = Image.new("L", rgb.size, 255)
        else:
            alpha = Image.open(os.path.join(al_out, name)).convert("L").crop(box)
            if kind == "binary":
                alpha = alpha.point(lambda v: 255 if v >= 128 else 0)
        Image.merge("RGBA", (*rgb.split(), alpha)).save(os.path.join(a.dst, name))

    shutil.rmtree(work, ignore_errors=True)
    print(f"wrote {len(jobs)} textures to {a.dst}")


if __name__ == "__main__":
    main()

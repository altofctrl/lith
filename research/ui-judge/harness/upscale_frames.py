"""Upscale captured frames identically for all builds, per X.3.

Frames are captured at the panel's native 320x170 and RGB565 colour depth. That
is a very small image to hand a judge, and the three judges resize inputs
differently in their own preprocessing, which would put a per-judge resampling
step inside the measurement path.

Nearest-neighbour, integer factor. Anything smoothing would blur the RGB565
banding and the 1px-stem bitmap glyphs into something the panel does not show,
which is the opposite of what a fidelity-sensitive judgement needs.

  python upscale_frames.py <src_dir> <dst_dir> [factor]
"""

import os
import sys

from PIL import Image


def main():
    src, dst = sys.argv[1], sys.argv[2]
    factor = int(sys.argv[3]) if len(sys.argv) > 3 else 4
    os.makedirs(dst, exist_ok=True)
    n = 0
    for fn in sorted(os.listdir(src)):
        if not fn.endswith(".png"):
            continue
        im = Image.open(os.path.join(src, fn)).convert("RGB")
        im = im.resize((im.width * factor, im.height * factor), Image.NEAREST)
        im.save(os.path.join(dst, fn))
        n += 1
    print(f"upscaled {n} frames x{factor} -> {dst}")


if __name__ == "__main__":
    main()

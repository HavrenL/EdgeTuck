"""Encode captured documentation frames; requires Pillow, not an app dependency."""
import argparse
import csv
from pathlib import Path
from PIL import Image


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("capture", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    with (args.capture / "timing.csv").open(encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    samples = []
    for i in [0, 30, 60, 100, len(rows) - 1]:
        with Image.open(args.capture / "frames" / f"{i:04}.bmp") as frame:
            samples.append(frame.convert("RGB").resize((320, 200)))
    swatches = Image.new("RGB", (320, 200 * len(samples)))
    for i, sample in enumerate(samples):
        swatches.paste(sample, (0, i * 200))
    palette = swatches.quantize(colors=224)
    # Small white labels/icons occupy few pixels in the background-heavy scene.
    # Reserve neutral and folder colors so palette fitting cannot tint them.
    accents = [(x,x,x) for x in range(0,256,17)] + [
        (255,255,255),(250,250,250),(240,245,240),(230,238,233),
        (255,205,70),(255,218,105),(255,228,130),(231,209,160),
        (16,42,53),(43,106,111),(60,130,128),(80,148,145),
        (155,190,185),(189,215,209),(225,239,232),(40,117,86),
    ]
    palette.putpalette(palette.getpalette()[:224*3] + [c for rgb in accents for c in rgb])
    frames = []
    durations = []
    for i, row in enumerate(rows):
        with Image.open(args.capture / "frames" / f"{int(row['frame']):04}.bmp") as raw:
            image = raw.convert("RGB").resize((960, 600), Image.Resampling.LANCZOS)
            frames.append(image.quantize(palette=palette, dither=Image.Dither.FLOYDSTEINBERG))
        elapsed = int(rows[i+1]["milliseconds"]) - int(row["milliseconds"]) if i+1 < len(rows) else 1000
        durations.append(max(20, round(elapsed / 10) * 10))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    frames[0].save(args.output, save_all=True, append_images=frames[1:], duration=durations, loop=0, optimize=True, disposal=1)
    with Image.open(args.output) as result:
        print(f"{result.n_frames} frames, {result.size}, {args.output.stat().st_size} bytes")


if __name__ == "__main__":
    main()

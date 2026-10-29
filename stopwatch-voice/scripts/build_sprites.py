#!/usr/bin/env python3
"""Pack zero to three local Codex 8x9 pets and optional four-row extra.png into the 2MB partition.

uv run --with pillow scripts/build_sprites.py --report /absolute/path/to/evidence.json
Only resampling, palette quantization and alpha extraction happen here; no new art is drawn.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
WIDTH, HEIGHT, ROWS, COLUMNS = 96, 104, 13, 8
COUNTS = (6, 8, 8, 4, 5, 8, 6, 6, 6, 8, 8, 8, 8)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--pets-dir", type=Path, default=ROOT / "pets")
    parser.add_argument("--pet", action="append", help="directory name, repeat to set selection order; default: sorted directories")
    args = parser.parse_args()
    names = args.pet if args.pet is not None else sorted(p.parent.name for p in args.pets_dir.glob("*/pet.json"))
    assert len(names) <= 3 and len(names) == len(set(names)), "supported pet count is 0..3"
    assert all(Path(name).name == name for name in names), "pet names must be local directory names"
    folder = ROOT / "assets/pets"
    palettes, pixels, report = [], [], []
    folder.mkdir(parents=True, exist_ok=True)
    for name in names:
        pet_folder = args.pets_dir / name
        metadata = json.loads((pet_folder / "pet.json").read_text())
        assert metadata["spritesheetPath"] == "spritesheet.webp", "expected Codex spritesheet.webp"
        original = pet_folder / "spritesheet.webp"
        extra = pet_folder / "extra.png"
        base = Image.open(original).convert("RGBA")
        added = Image.open(extra).convert("RGBA") if extra.exists() else None
        assert base.size == (1536, 1872), (name, base.size)
        # Supplemental outputs retain their complete image_gen canvas. Only map the grid to
        # the existing cell dimensions; never warp or invent individual poses.
        if added is not None:
            supplemental = added.resize((1536, 832), Image.Resampling.NEAREST)
        else:
            supplemental = Image.new("RGBA", (1536, 832))
            # Missing back/up/down use idle; anger uses the existing excited row.
            for row, source_row in enumerate((0, 3, 0, 0)):
                for frame in range(COLUMNS):
                    source_frame = frame % COUNTS[source_row]
                    supplemental.paste(base.crop((source_frame*192, source_row*208, (source_frame+1)*192, (source_row+1)*208)), (frame*192, row*208))
        combined = Image.new("RGBA", (1536, 2704))
        combined.paste(base, (0, 0))
        combined.paste(supplemental, (0, 1872))
        assert combined.crop((0, 0, 1536, 1872)).tobytes() == base.tobytes()
        small = combined.resize((WIDTH*COLUMNS, HEIGHT*ROWS), Image.Resampling.NEAREST)
        alpha = small.getchannel("A")
        # Quantize opaque foreground alone so transparent RGB never consumes the palette.
        foreground = [rgb for rgb, a in zip(small.convert("RGB").getdata(), alpha.getdata()) if a >= 128]
        assert foreground, f"empty pet: {name}"
        strip = Image.new("RGB", (len(foreground), 1))
        strip.putdata(foreground)
        quantized = strip.quantize(colors=15, method=Image.Quantize.MEDIANCUT)
        palette = quantized.getpalette()[:45]
        packed_palette = [0xf81f]
        for r, g, b in zip(palette[0::3], palette[1::3], palette[2::3]):
            c = (r>>3)<<11 | (g>>2)<<5 | b>>3
            assert c != 0xf81f, "sprite colour collides with transparent key"
            packed_palette.append(c)
        packed_palette += [0] * (16-len(packed_palette))
        palettes.append(struct.pack("<16H", *packed_palette))
        indexed = small.convert("RGB").quantize(palette=quantized, dither=Image.Dither.NONE)
        pet_pixels, cells = bytearray(), []
        for row, count in enumerate(COUNTS):
            for frame in range(COLUMNS):
                box = (frame*WIDTH, row*HEIGHT, (frame+1)*WIDTH, (row+1)*HEIGHT)
                rgba = small.crop(box)
                mask = rgba.getchannel("A").point(lambda a: 255 if a>=128 else 0)
                bbox = mask.getbbox()
                if frame < count:
                    assert bbox, (name, row, frame, "empty required cell")
                    cells.append({"row": row, "frame": frame, "bbox": bbox,
                                  "edge_touch": bbox[0]==0 or bbox[1]==0 or bbox[2]==WIDTH or bbox[3]==HEIGHT,
                                  "opaque_pixels": sum(a!=0 for a in mask.getdata())})
                indices = list(indexed.crop(box).getdata())
                assert all(i < 15 for i in indices)
                indices = [i+1 if a>=128 else 0 for i, a in zip(indices, rgba.getchannel("A").getdata())]
                pet_pixels.extend((a<<4)|b for a,b in zip(indices[::2],indices[1::2]))
        pixels.append(pet_pixels)
        report.append({"pet": name, "original_sha256": hashlib.sha256(original.read_bytes()).hexdigest(),
                       "supplement_sha256": hashlib.sha256(extra.read_bytes()).hexdigest() if extra.exists() else None,
                       "supplement_size": added.size if added else None, "original_rows_pixel_preserved": True,
                       "cells": cells, "edge_touch_cells": sum(c["edge_touch"] for c in cells)})
    data = b"SWPET001" + struct.pack("<4H", WIDTH, HEIGHT, ROWS, COLUMNS) + b"".join(palettes) + b"".join(pixels)
    assert len(data) <= 0x200000
    (folder / "sprites.bin").write_bytes(data)
    result = {"bytes": len(data), "partition_bytes": 0x200000, "sha256": hashlib.sha256(data).hexdigest(), "pets": report}
    (folder / "manifest.json").write_text(json.dumps({"count": len(names), "sha256": result["sha256"]}) + "\n")
    args.report.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"bytes": len(data), "edge_touch_cells": {r["pet"]: r["edge_touch_cells"] for r in report}}))


if __name__ == "__main__":
    main()

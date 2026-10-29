#!/usr/bin/env python3
"""Discard alpha<40 noise, then use nearest opaque foreground RGB for surviving edges.

This is the parent-requested deterministic removal of red matte contamination. Originals
remain untouched. Alpha changes only below the approved cutoff. Extract at transparent gutters,
feet at approximate quarter-image boundaries. Comparison is at the firmware's 0.85x96x104.
uv run --with pillow --with numpy --with scipy scripts/clean_sprite_edges.py --output DIR
"""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image
from scipy.ndimage import distance_transform_edt, binary_erosion

ROOT = Path(__file__).resolve().parents[1]
PETS = tuple(p.name.removesuffix("-extra.png") for p in sorted((ROOT / "assets/pets").glob("*-extra.png")))


def boundaries(profile, count):
    empty = np.flatnonzero(profile == 0)
    assert len(empty), "no transparent gutters"
    cuts = [0]
    for i in range(1, count):
        ideal = len(profile) * i / count
        cuts.append(int(empty[np.argmin(abs(empty-ideal))]))
    cuts.append(len(profile))
    assert all(a < b for a,b in zip(cuts,cuts[1:])), cuts
    return cuts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    reports = []
    for pet in PETS:
        source = ROOT / f"assets/pets/{pet}-extra.png"
        original = np.array(Image.open(source).convert("RGBA"))
        alpha = original[:,:,3]
        # image_gen's foreground plateau is alpha=250..254 (mode 253); literal 255 is
        # sparse and is not a valid colour anchor. Only change the surviving contour,
        # never recolour that foreground plateau or any interior detail.
        anchor_alpha = 250
        opaque = alpha >= anchor_alpha
        assert opaque.any(), f"no opaque grounding pixels: {pet}"
        # Parent's explicit 2026-10-29 correction: the generated background has widespread
        # faint alpha noise. Do not colour it. 40 is the parent-provided initial cutoff.
        cutoff = 40
        partial = (alpha >= cutoff) & (alpha < anchor_alpha) & ~binary_erosion(alpha >= 128)
        indices = distance_transform_edt(~opaque, return_distances=False, return_indices=True)
        clean = original.copy()
        clean[alpha < cutoff] = 0
        clean[partial,:3] = original[indices[0][partial],indices[1][partial],:3]
        assert np.array_equal(clean[:,:,3][alpha >= cutoff], alpha[alpha >= cutoff])
        assert not clean[:,:,3][alpha < cutoff].any()
        assert np.array_equal(clean[opaque], original[opaque])
        assert np.array_equal(clean[:,:,3] >= 128, alpha >= 128)
        assert (clean[:,:,3] <= alpha).all()
        Image.fromarray(clean).save(ROOT / f"assets/pets/{pet}-clean.png")
        occupied = alpha >= 128
        ys = boundaries(occupied.sum(axis=1), 4)
        atlases = [Image.new("RGBA", (1536,832)) for _ in range(2)]
        boxes = []
        for row in range(4):
            xs = boundaries(occupied[ys[row]:ys[row+1]].sum(axis=0), 8)
            for col in range(8):
                box = (xs[col],ys[row],xs[col+1],ys[row+1])
                boxes.append(box)
                for dest, pixels in zip(atlases, (original,clean)):
                    cell = Image.fromarray(pixels).crop(box)
                    # Fit the complete source cell within the original 192x208 geometry.
                    # One firmware pixel of transparent padding on all sides survives 2x reduction.
                    cell.thumbnail((192-2*(192//96),208-2*(208//104)),Image.Resampling.NEAREST)
                    dest.paste(cell,(col*192+(192-cell.width)//2,row*208+(208-cell.height)//2))
        atlases[1].save(ROOT / f"assets/pets/{pet}-extra-cells.png")
        comparison = Image.new("RGB", (8*82,4*2*88), "black")
        for row in range(4):
            for col in range(8):
                for side, atlas in enumerate(atlases):
                    cell=atlas.crop((col*192,row*208,(col+1)*192,(row+1)*208)).resize((82,88),Image.Resampling.NEAREST)
                    comparison.paste(cell,(col*82,(row*2+side)*88),cell.getchannel("A"))
        comparison.save(args.output / f"{pet}-black-before-after.png")
        changed = np.any(original[:,:,:3] != clean[:,:,:3],axis=2) & partial
        reports.append({"pet":pet,"partial_pixels":int(partial.sum()),"rgb_changed_pixels":int(changed.sum()),
                        "alpha_cutoff":cutoff,"foreground_anchor_alpha":anchor_alpha,
                        "removed_faint_pixels":int(((alpha>0)&(alpha<cutoff)).sum()),
                        "surviving_alpha_preserved":True,"opaque_pixels_preserved":True,"source_boxes":boxes,
                        "firmware_silhouette_changed_pixels":int(np.count_nonzero((clean[:,:,3]>=128)!=(alpha>=128))),
                        "newly_visible_pixels":int(np.count_nonzero((clean[:,:,3]>0)&(alpha==0))),
                        "comparison":"each pair of rows: before above, cleaned below; 82x88 per sprite (firmware scale)",
                        "method":"alpha<40 cleared; surviving contour only uses nearest alpha>=250 foreground colour; foreground/interior unchanged"})
    (args.output / "edge-cleanup.json").write_text(json.dumps(reports,indent=2)+"\n")
    print(json.dumps([{k:v for k,v in r.items() if k!='source_boxes'} for r in reports]))


if __name__ == "__main__":
    main()

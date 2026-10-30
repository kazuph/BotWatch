#!/usr/bin/env python3
"""Append missing caption glyphs, retaining every existing glyph and metric.

uv run --with pillow --with fonttools scripts/extend_ui_font.py
Uses the already pinned font source and hash in rounded-source.json.
"""
import hashlib
import json
import math
import re
import struct
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
from fontTools.ttLib import TTFont

ROOT = Path(__file__).resolve().parents[1]


def extend(font_size,weight,required):
    target=ROOT / f"assets/ui{font_size}.vlw"
    data=target.read_bytes()
    count,version,size,zero,ascent,descent=struct.unpack_from(">6i",data)
    metrics=[struct.unpack_from(">7i",data,24+i*28) for i in range(count)]
    bitmaps=data[24+count*28:]
    assert len(bitmaps)==sum(m[1]*m[2] for m in metrics)
    manifest_path=ROOT / "assets/rounded-source.json"
    manifest=json.loads(manifest_path.read_text())
    fontpath=ROOT / f"models/fonts/rounded/MPLUSRounded1c-{weight}.ttf"
    assert hashlib.sha256(fontpath.read_bytes()).hexdigest()==manifest["fonts"][str(weight)]
    font=ImageFont.truetype(str(fontpath),size)
    cmap=TTFont(fontpath).getBestCmap()
    assert all(ord(c) in cmap for c in required if c != " "), "missing source glyph"
    additions=[]
    for char in sorted(set(required)-{" "}-{chr(m[0]) for m in metrics}):
        left,top,right,bottom=font.getbbox(char,anchor="ls")
        width,height=right-left,bottom-top
        ascent=max(ascent,-top)
        descent=max(descent,height+top)
        bitmap=Image.new("L",(width,height))
        ImageDraw.Draw(bitmap).text((-left,-top),char,font=font,anchor="ls",fill=255)
        assert bitmap.getbbox()
        metrics.append((ord(char),height,width,math.ceil(font.getlength(char)),-top,left,0))
        bitmaps+=bitmap.tobytes()
        additions.append(char)
    result=struct.pack(">6i",len(metrics),version,size,zero,ascent,descent)
    # M5GFX searches the Unicode table in order. Keep each bitmap with its record.
    glyphs=[]
    offset=0
    for metric in metrics:
        length=metric[1]*metric[2]
        glyphs.append((metric,bitmaps[offset:offset+length]))
        offset+=length
    assert offset==len(bitmaps)
    glyphs.sort(key=lambda item:item[0][0])
    assert len({item[0][0] for item in glyphs})==len(glyphs)
    result+=b"".join(struct.pack(">7i",*m) for m,_ in glyphs)+b"".join(bitmap for _,bitmap in glyphs)
    target.write_bytes(result)
    record=next(r for r in manifest["fonts_by_size"] if r["size"]==font_size)
    record.update(ascent=ascent,descent=descent,glyphs=len(metrics),bytes=len(result),sha256=hashlib.sha256(result).hexdigest(),characters="".join(sorted(chr(m[0]) for m in metrics)))
    manifest_path.write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+"\n")
    include=ROOT / "main/ui_fonts.inc"
    include.write_text(re.sub(r"\{"+str(font_size)+r", \d+, ui"+str(font_size)+r"_start\}",f"{{{font_size}, {ascent}, ui{font_size}_start}}",include.read_text()))
    print(json.dumps({"size":font_size,"added":additions,"preserved_glyphs":count,"ascent":ascent,"descent":descent},ensure_ascii=False))


def main():
    # Dynamic app values: numeric time/date, octave, Hz and cents.
    for size,chars in {17:"0123456789%",20:"ラップリセットとめるはじめる",24:"0123456789:.+-/Hzcentむせんさがしていますつながりましたはなしていますきいていますむせんをはじめられません",32:"0123456789.Hz",40:"0123456789:.",56:"0123456789"}.items():
        extend(size,700 if size<=24 else 800,chars)
    sample_names="".join(re.findall(r'"([^"\n]+)"',(ROOT / "main/presets_local.example.h").read_text()))
    # Local display names use kana; keep a general kana alphabet in the small UI sizes.
    kana="".join(chr(c) for c in range(0x3041,0x3097))+"".join(chr(c) for c in range(0x30a1,0x30f7))
    for size in (15,17):extend(size,700,sample_names+kana)
    replies="".join(re.findall(r'\{"[^"\n]+", "([^"\n]+)"\}', (ROOT / "main/talk_commands.h").read_text()))
    extend(32,800,"ジャンプ！あそぼう！うしろをむいたよ背景をかえたよ"+sample_names+replies)


if __name__=="__main__": main()

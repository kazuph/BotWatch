#!/usr/bin/env python3
"""Numerically compare native VLW glyph ink with captured tuner scanout."""
import argparse,json,struct
from pathlib import Path
from PIL import Image
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--screens',required=True,type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[1];data=(root/'assets/ui32.vlw').read_bytes();count,_,size,_,ascent,_=struct.unpack_from('>6i',data);offset=24+count*28;glyphs={}
for i in range(count):
 code,h,w,advance,top,left,_=struct.unpack_from('>7i',data,24+i*28);glyphs[chr(code)]=(h,w,advance,top,left,data[offset:offset+h*w]);offset+=h*w
ja=['ド','ド♯ / レ♭','レ','レ♯ / ミ♭','ミ','ファ','ファ♯ / ソ♭','ソ','ソ♯ / ラ♭','ラ','ラ♯ / シ♭','シ']
de=['C','Cis / Des','D','Dis / Es','E','F','Fis / Ges','G','Gis / As','A','B','H'];results=[]
for language,notes in enumerate([de,ja]):
 for pc,text in enumerate(notes):
  image=Image.open(a.screens/f'tuner-{language}-{pc}.png').convert('RGB');origin=(image.width-466)//2;x=233+origin-sum(glyphs[c][2] for c in text)//2;checked=missing=0
  for c in text:
   h,w,advance,top,left,alpha=glyphs[c]
   for y in range(h):
    for col in range(w):
     if alpha[y*w+col]<128:continue
     point=(x+left+col,152+origin-top+y);pixel=image.getpixel(point);checked+=1
     # Native kInk dark text must survive RGB565 conversion on ivory.
     expected_max=(0x4a*(alpha[y*w+col]+1)+0xf7*(255-alpha[y*w+col]))>>8
     if max(pixel)>expected_max+(1<<(8-5)):missing+=1
   x+=advance
  assert checked and not missing,(language,pc,text,checked,missing)
  results.append({'language':language,'pitch_class':pc,'text':text,'checked_ink_pixels':checked,'missing':missing})
(a.screens/'glyph-proof.json').write_text(json.dumps(results,ensure_ascii=False,indent=2)+'\n');print(f'PASS native tuner glyph pixels across {len(results)} note labels')

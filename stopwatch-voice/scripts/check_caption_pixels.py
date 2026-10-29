#!/usr/bin/env python3
"""Verify saved device captions against the actual VLW glyph pixels and circular display.
Usage: uv run --with pillow python scripts/check_caption_pixels.py --cases evidence/summary.json --output proof.json
"""
import argparse,json,math,struct
from pathlib import Path
from PIL import Image


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cases',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    data=(Path(__file__).resolve().parents[1]/'assets/ui32.vlw').read_bytes()
    count,_,_,_,ascent,_=struct.unpack_from('>6i',data)
    offset=24+28*count;glyphs={}
    for i in range(count):
        code,h,w,advance,top,left,_=struct.unpack_from('>7i',data,24+28*i)
        glyphs[chr(code)]=(h,w,advance,top,left,data[offset:offset+h*w]);offset+=h*w
    proof=[]
    for case in json.loads(args.cases.read_text()):
        image=Image.open(case['png']).convert('RGB');width,height=image.size
        ink=(255,255,255) if case['background']=='black' else (74,69,49)
        for row in case['rows']:
            scale=int(row['scale']*65536);cursor=int(row['x']);y=int(row['y'])
            core=set();visible=set()
            for char in row['text']:
                assert char in glyphs,('missing glyph',char)
                h,w,advance,top,left,bitmap=glyphs[char]
                x=cursor+((left*scale)>>16)
                for py in range(h):
                    y0=y+(((ascent-top+py)*scale)>>16);y1=y+(((ascent-top+py+1)*scale)>>16)
                    for px in range(w):
                        alpha=bitmap[py*w+px]
                        if not alpha:continue
                        x0=x+((px*scale)>>16);x1=x+(((px+1)*scale)>>16)
                        for yy in range(y0,y1):
                            for xx in range(x0,x1):
                                visible.add((xx,yy))
                                if alpha==255:core.add((xx,yy))
                cursor+=(advance*scale)>>16
            assert core,('no glyph core',case['label'],row)
            outline=int(row['outline'])
            outside=sum((x+dx+.5-width/2)**2+(y+dy+.5-height/2)**2>(width/2)**2
                        for x,y in visible for dx,dy in [(0,0),(-outline,0),(outline,0),(0,-outline),(0,outline)])
            mismatch=sum(image.getpixel((x,y))!=ink for x,y in core)
            assert outside==0,(case['label'],'circle clips glyph',outside)
            assert mismatch==0,(case['label'],'missing/changed solid glyph pixels',mismatch,len(core))
            proof.append({'case':case['label'],'row':int(row['row']),'text':row['text'],'solid_pixels':len(core),
                          'missing_solid_pixels':mismatch,'glyph_pixels_outside_circle':outside,'scale':row['scale']})
    args.output.write_text(json.dumps(proof,ensure_ascii=False,indent=2)+'\n')
    print('PASS rows=',len(proof),'missing_solid_pixels=0 glyph_pixels_outside_circle=0')


if __name__=='__main__':main()

#!/usr/bin/env python3
"""Exercise the real app overlay and save device scanout; run under device_lock.py."""
import argparse
import json
import struct
import time
from pathlib import Path
import serial
from PIL import Image

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--port',required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--tuner',action='store_true')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
log=(a.output/'serial.log').open('wb')
port=serial.Serial(a.port,115200,timeout=1,write_timeout=10)
def until(prefix,seconds=120):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        raw=port.readline();log.write(raw);log.flush()
        line=raw.decode(errors='replace').strip()
        if line.startswith(prefix):return line
    raise TimeoutError(prefix)
def send(command,prefix):
    port.write(command);return until(prefix)
def frame(name):
    line=send(b'LFRM','SCREEN ');_,width,height=line.split();width,height=int(width),int(height)
    pixels=bytearray()
    for y in range(height):
        line=until('PIXELS ');_,row,data=line.split();assert int(row)==y
        raw=bytes.fromhex(data);assert len(raw)==width*3;pixels.extend(raw)
    until('SCREEN_END')
    image=Image.frombytes('RGB',(width,height),bytes(pixels));image.save(a.output/f'{name}.png')
    return {'width':width,'height':height,'colors':len(image.getcolors(width*height) or []),'bytes':len(pixels)}
results={}
try:
    results['muted']=send(b'TMUT','TTS_DIAGNOSTIC')
    results['menu']=send(b'APPS','APP menu')
    results['menu_screen']=frame('menu')
    if a.tuner:
        results['tuner']=send(b'ASEL\x02','APP current=2')
        results['tuner_screen']=frame('tuner')
        results['back']=send(b'ABAK','APP current=-1')
    # Return to current voice mode by reading INFO is not possible in the overlay;
    # tests explicitly choose action mode, which is the standard regression target.
    results['return']=send(b'ASEL\x00','APPS_EXIT')
finally:
    results['unmuted']=send(b'TLIV','TTS_DIAGNOSTIC')
    port.close();log.close()
    (a.output/'result.json').write_text(json.dumps(results,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(results,ensure_ascii=False))

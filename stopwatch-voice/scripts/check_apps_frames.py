#!/usr/bin/env python3
"""Capture the final firmware's app screens, with all sound muted and the microphone released for scanout."""
import argparse,array,json,math,struct,time
from pathlib import Path
from check_action import connect,info,until,screenshot
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
link=connect(a.port);log=(a.output/'serial.log').open('wb');r={};overlay=False

def send(cmd,prefix):link.write(cmd);return until(link,log,time.time()+120,lambda s:s.startswith(prefix))
def select(index):return send(b'ASEL'+bytes([index]),f'APP current={index}')
def inject(hz):
 values=array.array('h',[round(4000*math.sin(2*math.pi*hz*i/16000)) for i in range(3072)])
 for offset,count in [(0,2048),(2048,512),(2560,512)]:send(b'TPCM'+struct.pack('<H',count)+values[offset:offset+count].tobytes(),'TPCM_ACK ')
def frame(name):
 screenshot(link,log,a.output/f'{name}.png');r[name]=str(a.output/f'{name}.png')
try:
 boot=info(link,log);assert any(s.startswith('MODE action') for s in boot)
 send(b'FGON','FONT_AUDIT enabled=1');send(b'TMUT','TTS_DIAGNOSTIC');send(b'LHLD','LISTEN_HOLD 1');frame('voice')
 send(b'APPS','APP menu');overlay=True;frame('menu')
 select(2);inject(184.997211);frame('tuner');select(3);inject(440);frame('spectrum')
 select(4)
 for mode in range(3):send(b'EGAM'+bytes([mode]),'EAR_QUESTION ');frame(f'ear-{mode}')
 select(5);frame('stopwatch');select(6);frame('pomodoro');select(7);frame('clock')
 send(b'ASEL\x00','APPS_EXIT');overlay=False;send(b'LRUN','LISTEN_HOLD 0')
finally:
 if overlay:send(b'ASEL\x00','APPS_EXIT')
 send(b'FGOF','FONT_AUDIT enabled=0');send(b'TLIV','TTS_DIAGNOSTIC');link.close();log.close();(a.output/'result.json').write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(r,ensure_ascii=False),flush=True)

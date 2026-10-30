#!/usr/bin/env python3
"""Measure actual snapshot-restored versus redrawn frames for all four original screens."""
import argparse,json,re,struct,time
from pathlib import Path
from PIL import Image,ImageChops
from check_action import connect,info,until,screenshot,read_wav,send_rlma
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--home-wav',type=Path);p.add_argument('--output',required=True,type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
link=connect(a.port);log=(a.output/'serial.log').open('wb');result={};kana=False;opened=False

def send(data,prefix):link.write(data);return until(link,log,time.time()+120,lambda s:s.startswith(prefix))
def reopen(marker):
 global link
 link.close();link=connect(a.port,90);until(link,log,time.time()+120,lambda s:marker in s);info(link,log);send(b'TMUT','TTS_DIAGNOSTIC')
def roundtrip(name,mode,audio):
 global opened
 send(b'APPS','APP menu');opened=True
 if audio:send(b'ASEL\x03','APP current=3')
 send(b'ASEL'+bytes([mode]),'APPS_EXIT');opened=False
 screenshot(link,log,a.output/f'{name}.png');log.flush()
def difference(first,second,box=None):
 x=Image.open(first);y=Image.open(second)
 if box:x=x.crop(box);y=y.crop(box)
 d=ImageChops.difference(x,y);return {'pixels':sum(any(v) for v in d.getdata()),'bbox':d.getbbox()}
try:
 boot=info(link,log);assert 'MODE action partition=koeact' in boot;send(b'TMUT','TTS_DIAGNOSTIC');send(b'MODE','MODE_SWITCH ');kana=True;reopen('READY RLM1')
 for name,command in [('waiting',b'VWAI'),('people',b'VPPL'),('confirmation',b'VPST')]:
  send(command,'VIEW_READY ');screenshot(link,log,a.output/f'{name}-before.png')
  roundtrip(name+'-snapshot',1,False);roundtrip(name+'-redraw',1,True)
  actual=difference(a.output/f'{name}-snapshot.png',a.output/f'{name}-redraw.png');result[name]=actual;assert actual['pixels']==0,(name,actual)
 send(b'MODE','MODE_SWITCH ');kana=False;reopen('READY action mode');send(b'LHLD','LISTEN_HOLD 1')
 screenshot(link,log,a.output/'voice-before.png');roundtrip('voice-snapshot',0,False);roundtrip('voice-redraw',0,True)
 result['voice']={'full':difference(a.output/'voice-snapshot.png',a.output/'voice-redraw.png'),'caption_footer':difference(a.output/'voice-snapshot.png',a.output/'voice-redraw.png',(70,385,398,455)),'explanation':'character animation advances during both exits; compare the stationary caption/footer separately'}
 assert result['voice']['caption_footer']['pixels']==0,result['voice']
 if a.home_wav:
  send_rlma(link,read_wav(a.home_wav));send(b'','HOME_ACTION ');send(b'','TTS_LISTEN state=restored');roundtrip('home-snapshot',0,False);roundtrip('home-redraw',0,True)
  result['home']=difference(a.output/'home-snapshot.png',a.output/'home-redraw.png');assert result['home']['pixels']==0,result['home']
 result['status']='PASS'
finally:
 if opened:send(b'ASEL'+bytes([1 if kana else 0]),'APPS_EXIT')
 if kana:send(b'MODE','MODE_SWITCH ');reopen('READY action mode')
 send(b'LRUN','LISTEN_HOLD 0');send(b'TLIV','TTS_DIAGNOSTIC');link.close();log.close();(a.output/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(result,ensure_ascii=False))

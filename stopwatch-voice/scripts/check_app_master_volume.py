#!/usr/bin/env python3
"""Real TTS_NORMALIZE master before entry and after all four ear volume settings."""
import argparse,json,re,time
from pathlib import Path
from check_action import connect,info,until,read_wav,send_rlma
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--wav',required=True,type=Path);p.add_argument('--output',required=True,type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
link=connect(a.port);log=(a.output/'serial.log').open('wb');r={'cases':[]};opened=False

def send(data,prefix):link.write(data);return until(link,log,time.time()+120,lambda s:s.startswith(prefix))
def speak():
 send_rlma(link,read_wav(a.wav));line=send(b'','TTS_NORMALIZE ');send(b'','ACTION_DONE ');return int(re.search(r'master=(\d+)',line)[1])
def enter():
 global opened
 send(b'FGON','FONT_AUDIT enabled=1');send(b'APPS','APP menu');opened=True;send(b'ASEL\x04','APP current=4');log.flush()
 texts=[bytes.fromhex(s).decode() for s in re.findall(r'FONT_TEXT size=17 missing=0 utf8=([0-9a-f]+)',(a.output/'serial.log').read_text(errors='replace'))]
 return int(next(re.search(r'(\d+)%',s)[1] for s in reversed(texts) if s.startswith('おんりょう ')))
def change():return int(re.search(r'percent=(\d+)',send(b'EVOL','EAR_VOLUME '))[1])
def close():
 global opened
 send(b'FGOF','FONT_AUDIT enabled=0');send(b'ASEL\x00','APPS_EXIT');opened=False
try:
 info(link,log);send(b'TMUT','TTS_DIAGNOSTIC');send(b'LHLD','LISTEN_HOLD 1');r['before']=speak();original=None
 for target in (0,25,50,100):
  current=enter()
  if original is None:original=current;assert original in (0,25,50,100)
  for _ in range(4):
   if current==target:break
   current=change()
  assert current==target;close();actual=speak();r['cases'].append({'ear_percent':target,'tts_master':actual});assert actual==r['before']
 current=enter()
 for _ in range(4):
  if current==original:break
  current=change()
 assert current==original;close();r['status']='PASS'
finally:
 if opened:close()
 send(b'LRUN','LISTEN_HOLD 0');send(b'TLIV','TTS_DIAGNOSTIC');link.close();log.close();(a.output/'result.json').write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(r,ensure_ascii=False))

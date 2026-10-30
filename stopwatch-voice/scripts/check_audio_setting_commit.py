#!/usr/bin/env python3
"""Real NVS proof: pending audio volume survives only a clean app exit, never a reset."""
import argparse,json,re,subprocess,time
from pathlib import Path
from check_action import connect,info,until
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--output',required=True,type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
link=connect(a.port);log=(a.output/'serial.log').open('wb');r={};opened=False

def send(data,prefix):link.write(data);return until(link,log,time.time()+120,lambda s:s.startswith(prefix))
def reset():
 global link,opened
 link.close();subprocess.run(['uv','run','--with','esptool==5.6.0','python','-m','esptool','--chip','esp32s3','--port',a.port,'run'],check=True)
 link=connect(a.port,90);until(link,log,time.time()+90,lambda s:'READY action mode' in s);opened=False;info(link,log)
def volume():
 global opened
 send(b'TMUT','TTS_DIAGNOSTIC');send(b'LHLD','LISTEN_HOLD 1');send(b'FGON','FONT_AUDIT enabled=1');send(b'APPS','APP menu');opened=True;send(b'ASEL\x04','APP current=4')
 log.flush()
 texts=[bytes.fromhex(x).decode() for x in re.findall(r'FONT_TEXT size=17 missing=0 utf8=([0-9a-f]+)',(a.output/'serial.log').read_text(errors='replace'))]
 return int(next(re.search(r'(\d+)%',s)[1] for s in reversed(texts) if s.startswith('おんりょう ')))
def change():return int(re.search(r'percent=(\d+)',send(b'EVOL','EAR_VOLUME '))[1])
def close():
 global opened
 send(b'ASEL\x00','APPS_EXIT');opened=False
try:
 info(link,log);initial=volume();assert initial in (0,25,50,100),('initial custom volume cannot be restored with existing presets',initial)
 pending=change();r.update(initial=initial,pending=pending);reset();r['after_dirty_reset']=volume();assert r['after_dirty_reset']==initial
 assert change()==pending;send(b'ASEL\x05','APP current=5');close();reset();r['after_clean_exit']=volume();assert r['after_clean_exit']==pending
 for _ in range(4):
  if change()==initial:break
 else:raise AssertionError('volume restore failed')
 close();reset();r['restored']=volume();assert r['restored']==initial;close();r['status']='PASS'
finally:
 if opened:close()
 send(b'FGOF','FONT_AUDIT enabled=0');send(b'LRUN','LISTEN_HOLD 0');send(b'TLIV','TTS_DIAGNOSTIC');link.close();log.close();(a.output/'result.json').write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(r,ensure_ascii=False))

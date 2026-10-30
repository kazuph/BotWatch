#!/usr/bin/env python3
"""Capture and audit all nine preset styles and people/confirmation screens using real VLW draw logs."""
import argparse,json,time
from pathlib import Path
from check_action import connect,info,until,screenshot
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--output',required=True,type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
link=connect(a.port);log=(a.output/'serial.log').open('wb');r={};kana=False

def send(data,prefix):link.write(data);return until(link,log,time.time()+120,lambda s:s.startswith(prefix))
def reopen(marker):
 global link
 link.close();link=connect(a.port,90);until(link,log,time.time()+120,lambda s:marker in s);return info(link,log)
try:
 info(link,log);send(b'TMUT','TTS_DIAGNOSTIC');send(b'APPS','APP menu');send(b'ASEL\x01','MODE_SWITCH ');kana=True;reopen('READY RLM1');send(b'TMUT','TTS_DIAGNOSTIC');send(b'FGON','FONT_AUDIT enabled=1')
 send(b'VPPL','VIEW_READY ');screenshot(link,log,a.output/'people.png');send(b'VPST','VIEW_READY ');screenshot(link,log,a.output/'confirm.png')
 r['styles']=[]
 for style in range(9):r['styles'].append(send(b'LPRS'+bytes([style]),'RASTER '))
 screenshot(link,log,a.output/'styles.png');r['status']='PASS'
finally:
 send(b'FGOF','FONT_AUDIT enabled=0')
 if kana:send(b'MODE','MODE_SWITCH ');reopen('READY action mode')
 send(b'TLIV','TTS_DIAGNOSTIC');link.close();log.close();(a.output/'result.json').write_text(json.dumps({'styles':len(r.get('styles',[])),'status':r.get('status')},ensure_ascii=False,indent=2)+'\n')
print('captured people, print confirmation and all nine style draws')

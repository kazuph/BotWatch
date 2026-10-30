#!/usr/bin/env python3
"""Query the real printer after radio exit; only submit a job when status is non-normal (no paper setup)."""
import argparse,json,re,time
from pathlib import Path
from check_action import connect,info,until,read_wav,send_rlma
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--wav',required=True,type=Path);p.add_argument('--output',required=True,type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
link=connect(a.port);log=(a.output/'serial.log').open('wb');r={};kana=False

def send(data,prefix):link.write(data);return until(link,log,time.time()+120,lambda s:s.startswith(prefix))
def reopen(mode):
 global link
 link.close();link=connect(a.port,90);until(link,log,time.time()+120,lambda s:('READY RLM1' if mode=='kana' else 'READY action mode') in s)
 return info(link,log)
try:
 r['start']=info(link,log);assert any(s.startswith('MODE action') for s in r['start']);send(b'TMUT','TTS_DIAGNOSTIC');send(b'APPS','APP menu');send(b'ASEL\x01','MODE_SWITCH ');kana=True;r['kana']=reopen('kana');send(b'TMUT','TTS_DIAGNOSTIC')
 link.write(b'BLEQ');lines=[]
 while True:
  line=until(link,log,time.time()+120,lambda _:True);lines.append(line)
  if line.startswith('BLE_QUERY '):break
 r['query']=lines
 statuses=[int(re.search(r'code=([0-9a-f]+)',s)[1],16) for s in lines if s.startswith('BLE_STATUS ')]
 if not statuses:
  r['paper_test']='NOT_MET: printer not found; no job submitted'
 elif statuses[-1]==0:
  r['paper_test']='NOT_MET: normal status; need confirmed paper removal before submitting a print job'
 else:
  send_rlma(link,read_wav(a.wav));r['text']=send(b'','TEXT ');r['print']=send(b'PRNT','BLE_PRINT ')
  r['paper_test']='print failure path exercised; physical paper state must be established separately'
finally:
 if kana:
  send(b'MODE','MODE_SWITCH ');r['returned']=reopen('action')
 send(b'TLIV','TTS_DIAGNOSTIC');link.close();log.close();(a.output/'result.json').write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n')
print(json.dumps({k:v for k,v in r.items() if k not in ('start','kana','returned')},ensure_ascii=False))

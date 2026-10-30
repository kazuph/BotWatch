#!/usr/bin/env python3
"""Prove one-shot menu return and actual read-only NVS failure; preserve voice-print priority."""
import argparse,json,subprocess,time
from pathlib import Path
from check_action import connect,info,until,screenshot,read_wav,send_rlma
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--voice-start',required=True,type=Path);p.add_argument('--output',required=True,type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
link=connect(a.port);log=(a.output/'serial.log').open('wb');r={};mode='action';radio=False;menu=False

def send(data,prefix):link.write(data);return until(link,log,time.time()+120,lambda s:s.startswith(prefix))
def reopen(marker):
 global link
 link.close();link=connect(a.port,90);return until(link,log,time.time()+120,lambda s:marker in s)
def enter():
 global radio,menu
 send(b'APPS','APP menu');menu=True;send(b'ASEL\x08','MODE_SWITCH ');menu=False;radio=True;reopen('APP current=8');send(b'TMUT','TTS_DIAGNOSTIC')
def leave(name,failed=False):
 global radio,menu
 if failed:send(b'AFAL','APPS_RETURN_FAULT readonly=1')
 line=send(b'ABAK','MODE_SWITCH ');radio=False;r[name+'-switch']=line
 if failed:
  assert 'saved=ESP_ERR_NVS_READ_ONLY' in line and 'apps=0' in line
  reopen('READY action mode');r[name+'-boot']=info(link,log)
 else:
  assert 'saved=ESP_OK' in line and 'apps=1' in line
  reopen('APP menu');menu=True;screenshot(link,log,a.output/(name+'-menu.png'))
  send(b'ASEL'+bytes([0 if mode=='action' else 1]),'APPS_EXIT');menu=False;r[name+'-boot']=info(link,log)
 assert any(s.startswith('MODE '+mode) for s in r[name+'-boot'])
try:
 r['initial']=info(link,log);send(b'TMUT','TTS_DIAGNOSTIC');send(b'LHLD','LISTEN_HOLD 1');enter();leave('normal')
 # A hardware reset after consuming the return flag must remain in the waiting screen.
 link.close();subprocess.run(['uv','run','--with','esptool==5.6.0','python','-m','esptool','--chip','esp32s3','--port',a.port,'run'],check=True)
 reopen('READY action mode');r['one_shot_boot']=info(link,log);assert any(s.startswith('MODE action') for s in r['one_shot_boot']);send(b'TMUT','TTS_DIAGNOSTIC');send(b'LHLD','LISTEN_HOLD 1')
 enter();leave('read_only_failure',True)
 send(b'TMUT','TTS_DIAGNOSTIC');send(b'LHLD','LISTEN_HOLD 1');send_rlma(link,read_wav(a.voice_start));r['voice_start']=send(b'','MODE_SWITCH ');assert 'voice_print=1' in r['voice_start'];mode='kana';reopen('UI_STATE voice_print_words');send(b'TMUT','TTS_DIAGNOSTIC');send(b'LHLD','LISTEN_HOLD 1')
 enter();leave('voice_print');assert any('NVS init=ESP_OK voice_print=1' in s for s in r['voice_print-boot'])
 send(b'MODE','MODE_SWITCH ');mode='action';reopen('READY action mode');r['returned']=info(link,log);r['status']='PASS'
finally:
 if radio:
  send(b'ABAK','MODE_SWITCH ');reopen('APP menu');menu=True
 if menu:send(b'ASEL'+bytes([0 if mode=='action' else 1]),'APPS_EXIT')
 send(b'TLIV','TTS_DIAGNOSTIC');link.close();log.close();(a.output/'result.json').write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n')
print(json.dumps({k:v for k,v in r.items() if k.endswith('-switch') or k in ('voice_start','status')},ensure_ascii=False))

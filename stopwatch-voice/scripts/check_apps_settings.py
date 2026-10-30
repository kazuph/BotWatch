#!/usr/bin/env python3
"""NVS persistence, both voice modes, and exact preview restoration on the real board."""
import argparse,array,json,math,struct,time
from pathlib import Path
from PIL import Image,ImageChops
from check_action import connect,info,until,screenshot,read_wav,send_rlma
import serial
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--output',required=True,type=Path);p.add_argument('--kana-wav',required=True,type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
link=connect(a.port);log=(a.output/'serial.log').open('wb');r={}
def send(data,prefix):link.write(data);return until(link,log,time.time()+120,lambda s:s.startswith(prefix))
def reopen():
 global link
 link.close();link=connect(a.port,90)
 deadline=time.time()+90
 while True:
  try:
   until(link,log,deadline,lambda s:'READY ' in s or s.startswith('UI_STATE action_listening'))
   break
  except serial.SerialException:
   link.close();link=connect(a.port,90)
 boot=info(link,log);send(b'TMUT','TTS_DIAGNOSTIC');return boot
try:
 r['start']=info(link,log);send(b'TMUT','TTS_DIAGNOSTIC')
 if any(x.startswith('MODE action') for x in r['start']):
  send(b'APPS','APP menu');send(b'ASEL\x02','APP current=2');send(b'TCAL'+struct.pack('<H',442),'TCAL ');send(b'TNAM\x00','TNAM ')
  r['to_kana']=send(b'ASEL\x01','MODE_SWITCH ');r['kana_boot']=reopen();assert any(x.startswith('MODE kana') for x in r['kana_boot'])
 else:assert any(x.startswith('MODE kana') for x in r['start'])
 send(b'APPS','APP menu');send(b'ASEL\x02','APP current=2');v=array.array('h',[round(4000*math.sin(2*math.pi*442*i/16000)) for i in range(3072)])
 for start,count in [(0,2048),(2048,512),(2560,512)]:
  link.write(b'TPCM'+struct.pack('<H',count)+v[start:start+count].tobytes())
  line=until(link,log,time.time()+120,lambda s:s.startswith('TUNER '));send(b'', 'TPCM_ACK ')
 r['restored_tuner']=line;assert 'reference=442' in line and 'valid=1' in line and 'midi=69' in line
 screenshot(link,log,a.output/'persisted-german-a.png');send(b'TCAL'+struct.pack('<H',440),'TCAL ');send(b'TNAM\x01','TNAM ');r['same_kana']=send(b'ASEL\x01','APPS_EXIT ');assert 'mode=0' in r['same_kana']
 pcm=read_wav(a.kana_wav);send_rlma(link,pcm);r['kana_text']=until(link,log,time.time()+120,lambda s:s.startswith('TEXT '));assert r['kana_text'].endswith('わらって')
 send(b'VPPL','VIEW_READY ');screenshot(link,log,a.output/'people-before.png');send(b'APPS','APP menu');send(b'ASEL\x01','APPS_EXIT ');screenshot(link,log,a.output/'people-after.png')
 diff=ImageChops.difference(Image.open(a.output/'people-before.png'),Image.open(a.output/'people-after.png'));assert diff.getbbox() is None;r['people_pixels_equal']=True
 send(b'VPST','VIEW_READY ');screenshot(link,log,a.output/'confirm-before.png');send(b'APPS','APP menu');send(b'ASEL\x01','APPS_EXIT ');screenshot(link,log,a.output/'confirm-after.png')
 assert ImageChops.difference(Image.open(a.output/'confirm-before.png'),Image.open(a.output/'confirm-after.png')).getbbox() is None;r['confirmation_pixels_equal']=True
 send(b'APPS','APP menu');r['to_action']=send(b'ASEL\x00','MODE_SWITCH ');r['action_boot']=reopen();assert any(x.startswith('MODE action') for x in r['action_boot'])
 send(b'APPS','APP menu');send(b'ASEL\x04','APP current=4');r['ear_volumes']=[send(b'EVOL','EAR_VOLUME ') for _ in range(4)];screenshot(link,log,a.output/'ear-volume.png');send(b'ASEL\x00','APPS_EXIT ')
finally:
 send(b'INFO','MEM serving');r['unmute']=send(b'TLIV','TTS_DIAGNOSTIC');link.close();log.close();(a.output/'result.json').write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n')
print(json.dumps({k:v for k,v in r.items() if not k.endswith('boot') and k!='start'},ensure_ascii=False),flush=True)

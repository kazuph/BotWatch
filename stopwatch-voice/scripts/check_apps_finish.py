#!/usr/bin/env python3
"""Final real-board timing, overlay restoration, clock and timer regression."""
import argparse,array,datetime,json,math,re,struct,time
from pathlib import Path
from check_action import connect,until,screenshot,info
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--output',required=True,type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
port=connect(a.port);log=(a.output/'serial.log').open('wb');r={};measurements=[]
def send(cmd,prefix,seconds=120):
 port.write(cmd)
 def accept(s):
  if s.startswith('TUNER '):measurements.append(dict(x.split('=') for x in s.split()[1:]))
  if s.startswith(('ERROR','abort()','Guru Meditation')):raise RuntimeError(s)
  return s.startswith(prefix)
 return until(port,log,time.time()+seconds,accept)
def select(i):return send(b'ASEL'+bytes([i]),f'APP current={i}')
def feed(values):
 v=array.array('h',values);return send(b'TPCM'+struct.pack('<H',len(v))+v.tobytes(),'TPCM_ACK ')
try:
 r['boot']=info(port,log);send(b'TMUT','TTS_DIAGNOSTIC');send(b'LHLD','LISTEN_HOLD 1')
 screenshot(port,log,a.output/'before-overlay.png');send(b'APPS','APP menu');screenshot(port,log,a.output/'menu.png');select(2)
 for f in (55,440,932.327523,4186.009):
  send(b'TRST','TRST');values=[round(4000*math.sin(2*math.pi*f*i/16000)) for i in range(2048+12*512)]
  feed(values[:2048])
  for j in range(12):feed(values[2048+j*512:2048+(j+1)*512])
 r['analysis']=measurements
 select(5);send(b'SWTO','SWTO ');r['stopwatch_start']=send(b'SWST','SWST ');screenshot(port,log,a.output/'stopwatch.png');send(b'SWLP','SWLP ')
 select(6);send(b'PMRS','PMRS');send(b'PMTO','PMTO ');r['pomodoro_started_host']=time.time();r['pomodoro']=send(b'PMST','PMST ');screenshot(port,log,a.output/'pomodoro.png')
 select(7);r['clock']=send(b'WGET','WGET ');screenshot(port,log,a.output/'clock.png')
 now=datetime.datetime.now();r['clock_save']=send(b'WSET'+struct.pack('<HBBBBB',now.year,now.month,now.day,now.hour,now.minute,now.second),'CLOCK_SAVE ');assert r['clock_save']=='CLOCK_SAVE result=1'
 select(5);r['stopwatch_reentry']=send(b'SWST','SWST ');send(b'SWTO','SWTO ');screenshot(port,log,a.output/'stopwatch-lap.png');r['pomodoro_background']=send(b'PMST','PMST ')
 send(b'ABAK','APP current=-1');r['return']=send(b'ASEL\x00','APPS_EXIT');screenshot(port,log,a.output/'after-overlay.png')
 # Held inference prevents any caption change while the overlay is open.
 from PIL import Image,ImageChops
 before=Image.open(a.output/'before-overlay.png');after=Image.open(a.output/'after-overlay.png')
 r['overlay_pixel_difference']=sum(1 for pixel in ImageChops.difference(before,after).getdata() if any(pixel))
 # Character animation continues, so compare the stationary status/footer only.
 r['footer_difference_bbox']=ImageChops.difference(before.crop((70,385,398,455)),after.crop((70,385,398,455))).getbbox()
 assert r['footer_difference_bbox'] is None
 send(b'LRUN','LISTEN_HOLD 0')
finally:
 r['unmute']=send(b'TLIV','TTS_DIAGNOSTIC');port.close();log.close();(a.output/'result.json').write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n')
print(json.dumps({k:v for k,v in r.items() if k not in ('analysis','boot')},ensure_ascii=False))

#!/usr/bin/env python3
"""Measure pitch-change settling and silence hold in input samples on the real detector."""
import argparse,array,json,math,struct,time
from pathlib import Path
from check_action import connect,info,until
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--output',required=True,type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
link=connect(a.port);log=(a.output/'serial.log').open('wb');r={}
def send(cmd,prefix):link.write(cmd);return until(link,log,time.time()+120,lambda s:s.startswith(prefix))
def feed(values):
 v=array.array('h',values);link.write(b'TPCM'+struct.pack('<H',len(v))+v.tobytes());line=until(link,log,time.time()+120,lambda s:s.startswith('TUNER '));send(b'','TPCM_ACK ');return dict(x.split('=') for x in line.split()[1:])
def tone(f,count,offset=0):return [round(4000*math.sin(2*math.pi*f*i/16000)) for i in range(offset,offset+count)]
try:
 info(link,log);send(b'TMUT','TTS_DIAGNOSTIC');send(b'APPS','APP menu');send(b'ASEL\x02','APP current=2');send(b'TRST','TRST')
 feed(tone(440,2048));feed(tone(440,512,2048));r['before']=feed(tone(440,512,2560));assert r['before']['midi']=='69' and r['before']['valid']=='1'
 change=[]
 for hop in range(10):change.append(feed(tone(261.625565,512,hop*512)))
 r['change']=change;r['settled_hop']=next(i+1 for i,v in enumerate(change) if v['valid']=='1' and v['midi']=='60' and abs(float(v['cents']))<=2);assert r['settled_hop']<= (2048+3*512)//512
 zeros=[feed([0]*512) for _ in range(20)];r['silence']=zeros;r['cleared_hop']=next(i+1 for i,v in enumerate(zeros) if v['valid']=='0');assert r['cleared_hop']<=math.ceil((2048+8000)/512)
 r['settled_input_ms']=r['settled_hop']*32;r['cleared_input_ms']=r['cleared_hop']*32;r['return']=send(b'ASEL\x00','APPS_EXIT')
finally:
 r['unmute']=send(b'TLIV','TTS_DIAGNOSTIC');link.close();log.close();(a.output/'result.json').write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n')
print(json.dumps({k:v for k,v in r.items() if k not in ('change','silence')},ensure_ascii=False),flush=True)

#!/usr/bin/env python3
"""Inject stable/swept valid PCM and verify the displayed pitch's 0.5 s expiry."""
import argparse,array,json,math,struct,time
from pathlib import Path
from check_action import connect,info,until
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--output',required=True,type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
link=connect(a.port);log=(a.output/'serial.log').open('wb');r={};phase=0.;samples=0;opened=False

def send(data,prefix):link.write(data);return until(link,log,time.time()+120,lambda s:s.startswith(prefix))
def feed(count,sweep=False,silence=False):
 global phase,samples
 values=[]
 for _ in range(count):
  cents=(samples-3072)*20/512 if sweep else 0;phase+=2*math.pi*440*2**(cents/1200)/16000;values.append(0 if silence else round(6000*math.sin(phase)));samples+=1
 link.write(b'TPCM'+struct.pack('<H',count)+array.array('h',values).tobytes());line=send(b'','TUNER ');send(b'','TPCM_ACK ');return dict(x.split('=') for x in line.split()[1:])
try:
 info(link,log);send(b'TMUT','TTS_DIAGNOSTIC');send(b'LHLD','LISTEN_HOLD 1');send(b'APPS','APP menu');opened=True;send(b'ASEL\x02','APP current=2');send(b'TRST','TRST');feed(2048);feed(512);r['stable']=feed(512);assert r['stable']['valid']=='1'
 r['sweep']=[feed(512,True) for _ in range(32)];assert all(v['raw_valid']=='1' for v in r['sweep']);assert any(v['valid']=='0' for v in r['sweep']);assert all(v['valid']=='0' for v in r['sweep'] if int(v['display_age'])>=8000)
 r['restore']=[feed(512) for _ in range(7)];assert any(v['valid']=='1' and v['midi']=='69' for v in r['restore'])
 for _ in range(4):feed(512)
 r['silence']=[feed(512,silence=True) for _ in range(20)];assert r['silence'][-1]['valid']=='0';r['status']='PASS';send(b'ASEL\x00','APPS_EXIT');opened=False
finally:
 if opened:send(b'ASEL\x00','APPS_EXIT')
 send(b'LRUN','LISTEN_HOLD 0');send(b'TLIV','TTS_DIAGNOSTIC');link.close();log.close();(a.output/'result.json').write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n')
print('PASS stable/valid-sweep expiry/steady restore/silence hold')

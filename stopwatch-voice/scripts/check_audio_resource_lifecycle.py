#!/usr/bin/env python3
"""Check audio views are released on transitions while the boot-reserved arena remains unchanged."""
import argparse,json,re,time
from pathlib import Path
from check_action import connect,info,until
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--roundtrips',type=int,default=1);p.add_argument('--output',required=True,type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
link=connect(a.port);log=(a.output/'serial.log').open('wb');r={};opened=False

def send(cmd,prefix):link.write(cmd);return until(link,log,time.time()+120,lambda s:s.startswith(prefix))
try:
 r['before']=info(link,log);send(b'TMUT','TTS_DIAGNOSTIC');send(b'LHLD','LISTEN_HOLD 1');r['roundtrips']=[]
 for iteration in range(a.roundtrips):
  send(b'APPS','APP menu');opened=True
  for app in (3,2,4):
   send(b'ASEL'+bytes([app]),f'APP current={app}')
   # One second gives the real producer >15 windows/frames.
   time.sleep(1)
  send(b'ASEL\x05','APP current=5');send(b'ASEL\x00','APPS_EXIT');opened=False
  r['roundtrips'].append(info(link,log))
 r['after']=r['roundtrips'][-1]

finally:
 if opened:send(b'ASEL\x00','APPS_EXIT')
 send(b'LRUN','LISTEN_HOLD 0');send(b'TLIV','TTS_DIAGNOSTIC');link.close();log.close()
lines=(a.output/'serial.log').read_text(errors='replace').splitlines();r['release']=[dict((k,int(v)) for k,v in re.findall(r'(\w+)=(-?\d+)',s)) for s in lines if s.startswith('AUDIO_RELEASE ')]
for app in (2,3,4):
 values=[x for x in r['release'] if x['app']==app];assert len(values)==a.roundtrips
 for value in values:assert value['before_bytes']>0 and all(value[k]==0 for k in ('capture','window','spectrum','fft','playback')),value
r['captures']=[s for s in lines if s.startswith('AUDIO_CAPTURE ')];assert len(r['captures'])==2*a.roundtrips and all('failed=0' in s for s in r['captures'])
assert not any(s.startswith(('ERROR','Guru Meditation','abort()')) for s in lines)
r['memory']=[dict((k,int(v) if v.lstrip('-').isdigit() else v) for k,v in re.findall(r'(\w+)=([\w-]+)',s)) for s in lines if s.startswith('APP_MEMORY ')];assert len([v for v in r['memory'] if v['app']==3 and v['stage']=='allocated'])==a.roundtrips
reserved=r['memory'][0]['arena_reserved'];assert reserved>0
assert all(v['arena_reserved']==reserved and (v['arena_used']==0 if v['stage']=='before_allocate' else v['arena_used']<=v['arena_reserved']) for v in r['memory'])
assert all(int(re.search(r'psram_free=(\d+)',next(s for s in lines if s.startswith('MEM serving ')))[1])>=64*1024 for lines in r['roundtrips'])
r['status']='PASS';(a.output/'result.json').write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n');print(json.dumps({'status':r['status'],'release':r['release'],'captures':r['captures']},ensure_ascii=False))

#!/usr/bin/env python3
"""Real-board FPS, all 17 Action fixtures and 19 appliance fixtures; capture every TTS PCM."""
import argparse,json,re,struct,time
from pathlib import Path
from check_action import connect,info,until,read_wav,send_rlma,screenshot
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--fixtures',required=True,type=Path);p.add_argument('--output',required=True,type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
link=connect(a.port);log=(a.output/'serial.log').open('wb');r={'fps':[],'actions':[],'home':[]}
def send(data,prefix):link.write(data);return until(link,log,time.time()+120,lambda s:s.startswith(prefix))
def state():
 link.write(b'LSTA');text=' '.join(until(link,log,time.time()+30,lambda s:s.startswith(prefix)) for prefix in ('CHARACTER_STAT ','CHARACTER_TIMING ','CHARACTER_VIEW ','MEM listening '))
 return {k:float(v) for k,v in re.findall(r'(\w+)=([-\d.]+)',text)}
def gesture(kind,x=233,y=233,dx=0,dy=0):send(b'CTCH'+struct.pack('<BhhhhH',kind,x,y,dx,dy,50),'CHARACTER_DIAG ')
def settle():
 deadline=time.time()+30
 while True:
  value=state()
  if abs(value['slide_px'])<.5:return value
  assert time.time()<deadline,'slide did not settle'
try:
 r['boot']=info(link,log);assert any(s.startswith('MODE action') for s in r['boot']);send(b'TMUT','TTS_DIAGNOSTIC');send(b'LHLD','LISTEN_HOLD 1')
 v=settle()
 if v['group']:gesture(0,356,220)
 for _ in range(6):
  if settle()['selected']==0:break
  gesture(2,330,233,-200,0)
 for index,label in enumerate(['clawd','grok','felipe','group']):
  if index in (1,2):gesture(2,330,233,-200,0)
  if index==3:gesture(1)
  first=settle();end=time.monotonic()+10
  while time.monotonic()<end:
   raw=link.read(max(1,link.in_waiting))
   if raw:log.write(raw)
  last=state();fps=(last['frames']-first['frames'])*1e6/(last['elapsed_us']-first['elapsed_us'])
  screenshot(link,log,a.output/f'fps-{label}.png');r['fps'].append({'label':label,'first':first,'last':last,'fps':fps})
 gesture(0,356,220)
 rows=json.loads((a.fixtures/'action-regression.json').read_text());assert len(rows)==17
 for row in rows:
  path=a.fixtures/Path(row['wav']).name;send_rlma(link,read_wav(path));send(b'','KOEACT ');decoded=send(b'','KOEACT_JSON ').split(' ',1)[1]
  send(b'','KOEACT_KANA ');done=send(b'','ACTION_DONE ')
  shown=json.loads(decoded) if 'played=1' in done else [];ok=shown==row['expect']
  r['actions'].append({'label':row['label'],'got':shown,'expect':row['expect'],'ok':ok});assert ok,r['actions'][-1]
 for row in (a.fixtures/'tts_home/list.txt').read_text().splitlines():
  name,text,expected=row.split('\t');send_rlma(link,read_wav(a.fixtures/'tts_home'/f'{name}.wav'));send(b'','KOEACT_KANA')
  line=until(link,log,time.time()+120,lambda s:s.startswith(('HOME_ACTION ','ACTION_DONE ')))
  actual='none'
  if line.startswith('HOME_ACTION '):
   action=json.loads(line.split(' ',1)[1].rsplit(' caption=',1)[0]);actual=f"{action['device']} {action['command']} {action.get('step',action.get('value'))}"
   restored=send(b'','TTS_LISTEN state=restored');assert 'ok=1' in restored
  elif 'played=0' not in line:actual='koeact '+line.split('json=',1)[1]
  r['home'].append({'text':text,'got':actual,'expect':expected,'ok':actual==expected});assert actual==expected,r['home'][-1]
 r['status']='PASS'
finally:
 send(b'LRUN','LISTEN_HOLD 0');send(b'TLIV','TTS_DIAGNOSTIC');link.close();log.close();(a.output/'result.json').write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n')
print(json.dumps({'fps':[v['fps'] for v in r['fps']],'actions':len(r['actions']),'home':len(r['home']),'status':r.get('status')},ensure_ascii=False))

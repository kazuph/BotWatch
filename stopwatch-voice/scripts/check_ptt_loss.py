#!/usr/bin/env python3
"""Real TalkStop omission, 600ms recovery, actual failed sends/retries and byte-exact codec comparison."""
import argparse,array,json,math,re,struct,subprocess,time
from pathlib import Path
from check_action import connect,info,until
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--first',required=True);p.add_argument('--second',required=True);p.add_argument('--reference',required=True,type=Path);p.add_argument('--output',required=True,type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
ports=[a.first,a.second];links=[connect(x) for x in ports];logs=[(a.output/f'{i}.log').open('wb') for i in range(2)];opened=[False,False];r={}
def send(i,data,prefix):links[i].write(data);return until(links[i],logs[i],time.time()+120,lambda s:s.startswith(prefix))
def stat(i):
 send(i,b'INFO','MEM serving')
 return {k:int(v) for k,v in (x.split('=') for x in send(i,b'RSTA','RADIO_STAT ').split()[1:])}
def wait_for(fn,seconds=15):
 deadline=time.time()+seconds
 while True:
  value=fn()
  if value:return value
  assert time.time()<deadline,'state did not converge'
  time.sleep(.02)
def connect_pair():
 def check():
  values=[stat(i) for i in range(2)];return values if all(v['phase']==5 for v in values) and values[0]['session']==values[1]['session'] else None
 return wait_for(check)
def normal(i,menu=False):
 links[i].close();links[i]=connect(ports[i],90);until(links[i],logs[i],time.time()+90,lambda s:'READY action mode' in s)
 if menu:until(links[i],logs[i],time.time()+90,lambda s:s=='APP menu');send(i,b'ASEL\x00','APPS_EXIT')
 info(links[i],logs[i]);send(i,b'TMUT','TTS_DIAGNOSTIC');send(i,b'LHLD','LISTEN_HOLD 1');opened[i]=False
def enter(i):
 send(i,b'APPS','APP menu');send(i,b'ASEL\x08','MODE_SWITCH ');links[i].close();links[i]=connect(ports[i],90);until(links[i],logs[i],time.time()+90,lambda s:s=='APP current=8');send(i,b'TMUT','TTS_DIAGNOSTIC');opened[i]=True
values=array.array('h',[round(4000*math.sin(2*math.pi*440*n/16000)) for n in range(320)]);frame=values.tobytes();decoded=subprocess.run([str(a.reference.resolve())],input=frame,capture_output=True,check=True).stdout;assert len(decoded)==882*2

def packet(i):send(i,b'RPCM'+struct.pack('<H',320)+frame,'RADIO_PCM ')
def controlled_wave(talker):
 receiver=1-talker;send(talker,b'RKEY\x01','RADIO_KEY ')
 for _ in range(15):packet(talker);time.sleep(.02)
 def drained():
  v=stat(receiver);return v if v['decoded']==v['rx'] and v['rx']>=15 else None
 before=wait_for(drained);expected=before['hash']
 for _ in range(50):
  packet(talker);time.sleep(.02)
  for byte in decoded:expected=((expected^byte)*16777619)&0xffffffff
 after=wait_for(lambda:(v if (v:=stat(receiver))['decoded']>=before['decoded']+50 else None))
 assert after['hash']==expected and after['decoded']==before['decoded']+50,(after,expected)
 return {'talker':talker,'before':before,'after':after,'expected_hash':expected}
try:
 for i in range(2):info(links[i],logs[i]);send(i,b'TMUT','TTS_DIAGNOSTIC');send(i,b'LHLD','LISTEN_HOLD 1');enter(i)
 connected=connect_pair();parent=next(i for i,v in enumerate(connected) if v['role']==1);child=1-parent
 send(child,b'RDST','RADIO_DIAGNOSTIC armed_drop_talk_stop=1');r['forward']=controlled_wave(parent);send(parent,b'RKEY\x00','RADIO_KEY ')
 r['timeout']=send(child,b'','RADIO_REMOTE_TIMEOUT ');idle=int(re.search(r'idle_us=(\d+)',r['timeout'])[1]);assert 600000<=idle<=620000
 r['reply']=controlled_wave(child);send(child,b'RKEY\x00','RADIO_KEY ');wait_for(lambda:not stat(parent)['remote'])
 send(parent,b'RKEY\x01','RADIO_KEY ');packet(parent)
 logs[parent].flush();start=(a.output/f'{parent}.log').stat().st_size
 links[child].close();subprocess.run(['uv','run','--with','esptool==5.6.0','python','-m','esptool','--chip','esp32s3','--port',ports[child],'run'],check=True);opened[child]=False
 send(parent,b'RKEY\x00','RADIO_KEY ');r['failed_stop']=send(parent,b'','RADIO_TALK_STOP attempt=3 success=0');logs[parent].flush();text=(a.output/f'{parent}.log').read_bytes()[start:].decode(errors='replace');assert all(f'RADIO_TALK_STOP attempt={i} success=0' in text for i in (1,2,3))
 normal(child);enter(child);r['reconnected']=connect_pair();r['post_failure_reply']=controlled_wave(child);send(child,b'RKEY\x00','RADIO_KEY ');r['status']='PASS'
finally:
 for i in range(2):
  if opened[i]:send(i,b'ABAK','MODE_SWITCH ');normal(i,True)
  send(i,b'LRUN','LISTEN_HOLD 0');send(i,b'TLIV','TTS_DIAGNOSTIC');links[i].close();logs[i].close()
 (a.output/'result.json').write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n')
print(json.dumps({'status':r.get('status'),'timeout':r.get('timeout'),'failed_stop':r.get('failed_stop')},ensure_ascii=False))

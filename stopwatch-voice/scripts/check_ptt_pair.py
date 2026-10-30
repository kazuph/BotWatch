#!/usr/bin/env python3
"""Exercise actual two-board ESP-NOW PTT, with all audio muted. Holds one device_lock outside."""
import argparse,array,json,math,struct,time,subprocess,sys
from pathlib import Path
from check_action import connect,info,until,screenshot
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--frames',action='store_true');p.add_argument('--microphone',action='store_true');p.add_argument('--first',required=True);p.add_argument('--second',required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
ports=[a.first,a.second];links=[connect(x) for x in ports];logs=[(a.output/f'{i}.log').open('wb') for i in range(2)];result={};opened=[False,False]
def send(i,data,prefix):
 links[i].write(data);return until(links[i],logs[i],time.time()+120,lambda s:s.startswith(prefix))
def stats(i):
 line=send(i,b'RSTA','RADIO_STAT ');return {k:int(v) for k,v in (x.split('=') for x in line.split()[1:])}
def connected():
 deadline=time.time()+30
 while time.time()<deadline:
  values=[stats(i) for i in range(2)]
  if all(v['phase']==5 for v in values) and values[0]['session']==values[1]['session'] and {v['role'] for v in values}=={1,2}:return values
 raise AssertionError(('connection not established',values))
def radio_boot(i):
 links[i].close();links[i]=connect(ports[i],90)
 until(links[i],logs[i],time.time()+90,lambda s:s=='APP current=8')
 boot=info(links[i],logs[i]);assert 'MODE radio partition=none' in boot
 send(i,b'TMUT','TTS_DIAGNOSTIC');opened[i]=True
 if a.frames:send(i,b'FGON','FONT_AUDIT enabled=1')
 return boot
def normal_boot(i,menu=False):
 links[i].close();links[i]=connect(ports[i],90)
 until(links[i],logs[i],time.time()+90,lambda s:'READY action mode' in s)
 if menu:
  until(links[i],logs[i],time.time()+90,lambda s:s=='APP menu')
  if a.frames:screenshot(links[i],logs[i],a.output/f'returned-menu-{i}.png')
  send(i,b'ASEL\x00','APPS_EXIT')
 boot=info(links[i],logs[i]);assert any(s.startswith('MODE action') for s in boot)
 send(i,b'TMUT','TTS_DIAGNOSTIC');send(i,b'LHLD','LISTEN_HOLD 1');opened[i]=False
 return boot
def enter(i):
 send(i,b'APPS','APP menu');send(i,b'ASEL\x08','MODE_SWITCH ')
 return radio_boot(i)
def leave(i):
 send(i,b'ABAK','MODE_SWITCH ');return normal_boot(i,True)
try:
 for i in range(2):
  result[f'boot-{i}']=info(links[i],logs[i]);send(i,b'TMUT','TTS_DIAGNOSTIC');send(i,b'LHLD','LISTEN_HOLD 1');result[f'radio-boot-{i}']=enter(i)
  if a.frames and i==0:screenshot(links[i],logs[i],a.output/'radio-search.png')
  value=stats(i)
  assert value['phase']!=6,('radio startup failed',i,value)
 result['connected']=connected();result['directions']=[]
 if a.frames:screenshot(links[0],logs[0],a.output/'radio-connected.png')
 for talker in range(2):
  receiver=1-talker;before=stats(receiver);send(talker,b'RKEY\x01','RADIO_KEY ')
  values=array.array('h',[round(4000*math.sin(2*math.pi*(440+talker*220)*n/16000)) for n in range(320)])
  for frame in range(50):
   send(talker,b'RPCM'+struct.pack('<H',len(values))+values.tobytes(),'RADIO_PCM ')
   time.sleep(.02) # Real 20 ms capture-frame cadence, no speaker output.
  if a.frames:
   screenshot(links[talker],logs[talker],a.output/f'radio-talk-{talker}.png');screenshot(links[receiver],logs[receiver],a.output/f'radio-listen-{receiver}.png')
  send(talker,b'RKEY\x00','RADIO_KEY ')
  deadline=time.time()+10
  while True:
   received=stats(receiver);sender=stats(talker)
   if received['decoded']>=before['decoded']+50 and not received['remote'] and not sender['local'] and received['rx']==sender['tx']:break
   if time.time()>deadline:raise AssertionError(('missing decoded frames',before,received))
  assert received['rx']>=before['rx']+50 and received['drops']==0
  assert received['hash']!=before['hash']
  result['directions'].append({'talker':talker,'before':before,'receiver':received,'sender':sender})
 if a.microphone:
  result['microphone']=[]
  for talker in range(2):
   receiver=1-talker;base_tx=stats(talker);base_rx=stats(receiver);send(talker,b'RKEY\x01','RADIO_KEY ')
   deadline=time.time()+10
   while True:
    sending=stats(talker)
    if sending['tx']>=base_tx['tx']+50:break
    assert time.time()<deadline,('microphone capture did not transmit',sending)
   result[f'microphone-memory-{talker}']=info(links[talker],logs[talker])
   send(talker,b'RKEY\x00','RADIO_KEY ')
   deadline=time.time()+10
   while True:
    sending=stats(talker);received=stats(receiver)
    sent=sending['tx']-base_tx['tx'];got=received['rx']-base_rx['rx'];decoded=received['decoded']-base_rx['decoded']
    if not sending['local'] and not received['remote'] and sent==got==decoded and sent>=50:break
    assert time.time()<deadline,('microphone frames lost',sending,received)
   assert received['drops']==0 and sending['send_failures']==0
   result['microphone'].append({'talker':talker,'frames':sent,'sender':sending,'receiver':received})
 leave(1);result['peer_left']=stats(0);enter(1);result['reentry']=connected()
 # Reboot the peer through the standard esptool reset, then reconnect and reenter.
 links[1].close()
 subprocess.run(['uv','run','--with','esptool==5.6.0','python','-m','esptool','--chip','esp32s3','--port',ports[1],'run'],check=True)
 result['peer_reboot_boot']=normal_boot(1);enter(1);result['peer_restart']=connected()
 result['status']='PASS'
finally:
 for i in range(2):
  try:
   if opened[i]:leave(i)
   send(i,b'LRUN','LISTEN_HOLD 0');send(i,b'TLIV','TTS_DIAGNOSTIC')
  except Exception as error:result[f'cleanup-{i}']=str(error)
  links[i].close();logs[i].close()
 (a.output/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(result,ensure_ascii=False))

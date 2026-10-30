#!/usr/bin/env python3
"""Real-device PCM injection, app transitions, RTC and framebuffer evidence (USB lock required)."""
import argparse,array,datetime,json,math,random,re,struct,time
from pathlib import Path
import serial
from PIL import Image
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--resume',action='store_true');p.add_argument('--views-only',action='store_true')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
log=(a.output/'serial.log').open('wb');port=serial.Serial(a.port,115200,timeout=1,write_timeout=20)
results={};observed=[];pending=bytearray()
def until(prefix,seconds=120):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  while b'\n' in pending:
   split=pending.index(b'\n')+1;line=pending[:split].decode(errors='replace').strip();del pending[:split]
   if line.startswith('TUNER '):observed.append(dict(field.split('=') for field in line.split()[1:]))
   if line.startswith(('ERROR','Guru Meditation','abort()')):raise RuntimeError(line)
   if line.startswith(prefix):return line
  raw=port.read(max(1,port.in_waiting))
  if raw:log.write(raw);log.flush();pending.extend(raw)
 raise TimeoutError(prefix)

def send(cmd,prefix):port.write(cmd);return until(prefix)
def select(index):return send(b'ASEL'+bytes([index]),f'APP current={index}')
def frame(name):
 _,w,h=send(b'LFRM','SCREEN ').split();w,h=int(w),int(h);data=bytearray()
 for y in range(h):
  _,row,encoded=until('PIXELS ').split();assert int(row)==y;chunk=bytes.fromhex(encoded);assert len(chunk)==w*3;data.extend(chunk)
 until('SCREEN_END');im=Image.frombytes('RGB',(w,h),bytes(data));im.save(a.output/f'{name}.png')
 return {'width':w,'height':h,'bytes':len(data),'colors':len(im.getcolors(w*h) or [])}
def feed(values):
 samples=array.array('h',values)
 if struct.pack('=H',1)!=b'\x01\x00':samples.byteswap()
 port.write(b'TPCM'+struct.pack('<H',len(samples))+samples.tobytes());until('TPCM_ACK ')
 return observed[-1] if observed else None
def signal(f,n,kind,offset=0):
 out=[]
 for i in range(offset,offset+n):
  phase=2*math.pi*f*i/16000
  if kind=='noise':v=random.randrange(-10000,10001)
  elif kind=='dc':v=1000
  elif kind=='silence':v=0
  elif kind=='low':v=math.sin(phase)
  else:
   v=(300 if kind=='weak' else 4000)*math.sin(phase)
   if kind in ('harmonic','weak'):v+=9000*math.sin(2*phase)+3000*math.sin(3*phase)
   if kind=='decay':v*=math.exp(-i/2048)
   v+=1000
  out.append(round(v))
 return out
try:
 results['mute']=send(b'TMUT','TTS_DIAGNOSTIC');send(b'ABAK' if a.resume else b'APPS','APP current=-1' if a.resume else 'APP menu');results['menu']=frame('menu')
 select(2);quality=[];random.seed(42)
 if not a.views_only:
  for ref in (410,440,442,480):
   send(b'TCAL'+struct.pack('<H',ref),'TCAL ')
   for midi in (33,36,54,60,69,82,108):
    f=ref*2**((midi-69)/12)
    for kind in ('pure','harmonic','weak','decay','low'):
     send(b'TRST','TRST');started=time.monotonic();observed.clear()
     values=signal(f,3072,kind);feed(values[:2048]);feed(values[2048:2560]);last=feed(values[2560:])
     valid=int(last['valid'])==1;err=1200*math.log2(float(last['hz'])/f) if valid else None
     ok=not valid if kind=='low' else valid and int(last['midi'])==midi and abs(err)<=2
     quality.append({'reference':ref,'midi':midi,'kind':kind,'frequency':f,'result':last,'cents_error':err,'pass':ok,'host_elapsed_s':time.monotonic()-started,'stable_at_samples':3072 if valid else None})
     print(f'case ref={ref} midi={midi} kind={kind} pass={ok}',flush=True)
  results['quality']=quality;assert all(c['pass'] for c in quality),'tuner quality failure'
  for kind in ('silence','dc','noise'):
   send(b'TRST','TRST');observed.clear();values=signal(440,3072,kind);feed(values[:2048]);feed(values[2048:2560]);last=feed(values[2560:]);assert last['valid']=='0';results[kind]=last
  send(b'TCAL'+struct.pack('<H',440),'TCAL ')
  for cents in (20,-30):
   send(b'TRST','TRST');observed.clear();v=signal(440*2**(cents/1200),3072,'pure');feed(v[:2048]);feed(v[2048:2560]);last=feed(v[2560:]);assert abs(float(last['cents'])-cents)<=2;results[f'detune_{cents}']=last
 # Explicit Hz/note/octave screenshots in both notations, all pitch classes.
 for japanese in (0,1):
  send(b'TNAM'+bytes([japanese]),'TNAM ')
  for pc in range(12):
   send(b'TRST','TRST');v=signal(440*2**((60+pc-69)/12),3072,'pure');feed(v[:2048]);feed(v[2048:2560]);feed(v[2560:]);frame(f'tuner-{japanese}-{pc}')
 send(b'TNAM\x01','TNAM ')
 select(3);send(b'TRST','TRST');v=signal(440,2048,'pure');feed(v);results['spectrum']=frame('spectrum')
 select(4);results['ear']=[]
 for mode in range(3):
  line=send(b'EGAM'+bytes([mode]),'EAR_QUESTION ');correct=int(re.search(r'correct=(\d+)',line)[1]);wrong=(correct+1)%3
  results['ear'].append({'mode':mode,'question':line,'wrong':send(b'EANS'+bytes([wrong]),'EAR_ANSWER '),'right':send(b'EANS'+bytes([correct]),'EAR_ANSWER '),'screen':frame(f'ear-{mode}')})
 select(5);send(b'SWTO','SWTO ');results['stopwatch_start']=send(b'SWST','SWST ');frame('stopwatch-running');send(b'SWLP','SWLP ');select(6);send(b'PMRS','PMRS');send(b'PMTO','PMTO ');results['pomodoro_start_host']=time.time();results['pomodoro_start']=send(b'PMST','PMST ');frame('pomodoro');select(7);results['clock_before']=send(b'WGET','WGET ')
 now=datetime.datetime.now();send(b'WSET'+struct.pack('<HBBBBB',now.year,now.month,now.day,now.hour,now.minute,now.second),'CLOCK_SAVE ');results['clock_after']=send(b'WGET','WGET ');frame('clock')
 select(5);results['stopwatch_return']=send(b'SWST','SWST ');send(b'SWTO','SWTO ');frame('stopwatch-lap');results['pomodoro_background']=send(b'PMST','PMST ')
 select(-1) if False else send(b'ABAK','APP current=-1')
 results['return']=send(b'ASEL\x00','APPS_EXIT')
finally:
 results['unmute']=send(b'TLIV','TTS_DIAGNOSTIC');port.close();log.close();(a.output/'result.json').write_text(json.dumps(results,ensure_ascii=False,indent=2)+'\n')
print(json.dumps({k:v for k,v in results.items() if k!='quality'},ensure_ascii=False),flush=True)

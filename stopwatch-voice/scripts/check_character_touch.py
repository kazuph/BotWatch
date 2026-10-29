"""Real device traces through the same classifier as physical touches. Wrapped by device_lock."""
import argparse,json,re,struct,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from check_action import connect,info,until,screenshot
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port',required=True)
parser.add_argument('--output',required=True,type=Path)
args=parser.parse_args()
out=args.output;out.mkdir(parents=True,exist_ok=True)
records=[]
with (out/'serial.log').open('ab') as log:
 link=connect(args.port);info(link,log)
 link.write(b'TMUT');until(link,log,time.time()+15,lambda s:s=='TTS_DIAGNOSTIC muted=1 pcm=1')
 link.write(b'LHLD');until(link,log,time.time()+15,lambda s:s.startswith('LISTEN_HOLD 1'))
 def state():
  link.write(b'LSTA')
  metric=until(link,log,time.time()+20,lambda s:s.startswith('CHARACTER_STAT '))
  view=until(link,log,time.time()+20,lambda s:s.startswith('CHARACTER_VIEW '))
  return {k:float(v) for k,v in re.findall(r'(\w+)=([-\d.]+)',metric+' '+view)}
 def record(label,group,selected):
  deadline=time.monotonic()+15
  while True:
   value=state()
   if abs(value['slide_px'])<.5:break
   if time.monotonic()>deadline:raise TimeoutError('slide failed to settle to a half-pixel')
  assert int(value['group'])==group and int(value['selected'])==selected,(label,value)
  path=out/(label+'.png');screenshot(link,log,path)
  records.append({'label':label,'state':value,'png':str(path)})
  (out/'summary.json').write_text(json.dumps(records,indent=2))
  print(json.dumps(records[-1]),flush=True)
 def gesture(kind,x=233,y=233,dx=0,dy=0,duration=50):
  link.write(b'CTCH'+struct.pack('<BhhhhH',kind,x,y,dx,dy,duration))
  return until(link,log,time.time()+20,lambda s:s.startswith(('CHARACTER_DIAG ','ERROR')))
 # The selected character is loaded from NVS; an unselected new key starts at Clawd.
 initial=state();initial_index=int(initial['selected']);assert initial['group']==0
 record('00-initial-single',0,initial_index)
 gesture(2,330,233,-200,0,120);record('01-swipe-next',0,(initial_index+1)%6)
 gesture(2,330,233,-200,0,120);record('02-second-character',0,(initial_index+2)%6)
 gesture(2,130,233,200,0,120);record('03-swipe-previous',0,(initial_index+1)%6)
 gesture(1);record('04-double-tap-group',1,(initial_index+1)%6)
 gesture(0,356,220);record('05-group-tap-felipe',0,2)
 # A long hold must take the original mode switch route, which alone commits selection.
 line=gesture(3,233,233,0,0,1500);assert 'mode_hold=1' in line,line
 switched=until(link,log,time.time()+20,lambda s:s.startswith('MODE_SWITCH '));assert 'next=kana' in switched
 link.close();time.sleep(3);link=connect(args.port,60)
 boot=info(link,log);assert any(s.startswith('MODE kana') for s in boot)
 link.write(b'TMUT');until(link,log,time.time()+15,lambda s:s=='TTS_DIAGNOSTIC muted=1 pcm=1')
 screenshot(link,log,out/'06-kana-after-long-hold.png')
 link.write(b'MODE');until(link,log,time.time()+20,lambda s:s.startswith('MODE_SWITCH '))
 link.close();time.sleep(3);link=connect(args.port,60);boot=info(link,log)
 link.write(b'TMUT');until(link,log,time.time()+15,lambda s:s=='TTS_DIAGNOSTIC muted=1 pcm=1')
 assert any(s.startswith('MODE action') for s in boot)
 record('07-selection-restored-after-mode-reboot',0,2)
 link.close()

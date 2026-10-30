#!/usr/bin/env python3
"""Real recognition/TTS/home regression after leaving the new app overlay."""
import argparse,json,struct,time
from pathlib import Path
from check_action import connect,info,until,screenshot,read_wav,send_rlma
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--output',required=True,type=Path);p.add_argument('--clips',required=True,type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
link=connect(a.port);log=(a.output/'serial.log').open('wb');r=[]
def send(data,prefix):link.write(data);return until(link,log,time.time()+120,lambda s:s.startswith(prefix))
try:
 info(link,log);send(b'TMUT','TTS_DIAGNOSTIC');send(b'LHLD','LISTEN_HOLD 1')
 for clip,marker,expected in [('tts/k2.wav','ACTION_DONE ','"expression":"happy"'),('tts_home/h1.wav','HOME_ACTION ','"value":"on"')]:
  send_rlma(link,read_wav(a.clips/clip));line=until(link,log,time.time()+120,lambda s:s.startswith(marker));assert expected in line,line
  restored=until(link,log,time.time()+120,lambda s:s.startswith('TTS_LISTEN state=restored')) if marker=='HOME_ACTION ' else None
  if restored:assert 'ok=1' in restored
  screenshot(link,log,a.output/(Path(clip).stem+'.png'));r.append({'clip':clip,'result':line,'restored':restored})
 # The same hold trace used by physical touches must open the app overlay.
 line=send(b'CTCH'+struct.pack('<BhhhhH',3,233,233,0,0,1500),'CHARACTER_DIAG ');assert 'mode_hold=1' in line
 until(link,log,time.time()+30,lambda s:s=='APP menu');screenshot(link,log,a.output/'hold-menu.png');send(b'ASEL\x00','APPS_EXIT')
 send(b'LRUN','LISTEN_HOLD 0')
finally:
 send(b'INFO','MEM serving');send(b'TLIV','TTS_DIAGNOSTIC');link.close();log.close();(a.output/'result.json').write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(r,ensure_ascii=False),flush=True)

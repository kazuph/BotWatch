#!/usr/bin/env python3
"""Observe the full 25-minute focus interval and real M5IOE1 motor registers."""
import argparse,json,re,time
from pathlib import Path
from check_action import connect,info,until,screenshot
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--output',required=True,type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
link=connect(a.port);log=(a.output/'serial.log').open('wb');r={}
def send(cmd,prefix,seconds=120):link.write(cmd);return until(link,log,time.time()+seconds,lambda s:s.startswith(prefix))
try:
 info(link,log);send(b'TMUT','TTS_DIAGNOSTIC');send(b'LHLD','LISTEN_HOLD 1')
 send(b'TFAL','TTS_RESTORE_FAULT');send(b'APPS','APP menu');send(b'ASEL\x02','APP current=2');r['restore_failure']=send(b'ASEL\x00','APPS_EXIT');assert 'restored=0' in r['restore_failure']
 r['recovery']=until(link,log,time.time()+30,lambda s:s.startswith('MIC_RECOVERY '));assert 'ready=1' in r['recovery']
 send(b'APPS','APP menu');send(b'ASEL\x06','APP current=6');send(b'PMRS','PMRS');send(b'PMTO','PMTO ');r['started_host']=time.time();r['start']=send(b'PMST','PMST ');assert 'remaining=1500' in r['start'];send(b'ASEL\x07','APP current=7');screenshot(link,log,a.output/'clock-while-focus.png')
 (a.output/'result.json').write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n');print('POMODORO real focus interval started; observing serial phase change',flush=True)
 r['phase']=until(link,log,time.time()+1600,lambda s:s.startswith('POMODORO_PHASE '));r['phase_host']=time.time();assert 'phase=1 remaining=300 vibration=ESP_OK' in r['phase']
 word=int(re.search(r'motor_word=(-?\d+)',r['phase'])[1]);assert word==0x8000+(90*0xfff//255),(word,r['phase'])
 r['stopped']=until(link,log,time.time()+5,lambda s:s.startswith('POMODORO_VIBRATION '));assert 'stopped=1 motor_word=0' in r['stopped'];r['elapsed_host_seconds']=r['phase_host']-r['started_host']
 send(b'ASEL\x06','APP current=6');r['break']=send(b'PMST','PMST ');screenshot(link,log,a.output/'break-after-notification.png');send(b'PMRS','PMRS');r['return']=send(b'ASEL\x00','APPS_EXIT');send(b'LRUN','LISTEN_HOLD 0')
finally:
 r['unmute']=send(b'TLIV','TTS_DIAGNOSTIC');link.close();log.close();(a.output/'result.json').write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n')
print(json.dumps(r,ensure_ascii=False),flush=True)

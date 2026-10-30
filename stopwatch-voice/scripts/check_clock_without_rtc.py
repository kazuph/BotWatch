#!/usr/bin/env python3
"""Check actual RTC failure, build-time display glyphs/frame and responsive exit on the same board."""
import argparse,datetime,json,re,subprocess,time
from pathlib import Path
from check_action import connect,info,until,screenshot
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--object',type=Path,default=Path('build/esp-idf/main/CMakeFiles/__idf_main.dir/apps.cpp.obj'));p.add_argument('--output',required=True,type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
strings=subprocess.run(['strings',str(a.object)],capture_output=True,text=True,check=True).stdout.splitlines();dates=[s for s in strings if re.fullmatch(r'[A-Z][a-z]{2} [ 0-9][0-9] [0-9]{4}',s)];times=[s for s in strings if re.fullmatch(r'\d{2}:\d{2}:\d{2}',s)];assert len(dates)==len(times)==1
expected_date=datetime.datetime.strptime(dates[0],'%b %d %Y').strftime('%Y/%m/%d');expected_time=times[0]
link=connect(a.port);log=(a.output/'serial.log').open('wb');r={'compiled_date':dates[0],'compiled_time':expected_time,'expected_date':expected_date};opened=False

def send(data,prefix):link.write(data);return until(link,log,time.time()+120,lambda s:s.startswith(prefix))
try:
 r['boot']=info(link,log);assert 'MODE action partition=koeact' in r['boot'];send(b'TMUT','TTS_DIAGNOSTIC');send(b'LHLD','LISTEN_HOLD 1');send(b'FGON','FONT_AUDIT enabled=1');send(b'APPS','APP menu');opened=True;send(b'ASEL\x07','APP current=7')
 r['rtc_read']=send(b'WGET','ERROR WGET rtc');screenshot(link,log,a.output/'clock-build-time.png');r['rtc_read_again']=send(b'WGET','ERROR WGET rtc');screenshot(link,log,a.output/'clock-still-responsive.png');send(b'FGOF','FONT_AUDIT enabled=0');r['return']=send(b'ASEL\x00','APPS_EXIT');opened=False
 r['after']=info(link,log);assert 'MODE action partition=koeact' in r['after'];r['status']='PASS build-time date displayed and UI remained responsive; RTC read unresolved'
finally:
 if opened:send(b'FGOF','FONT_AUDIT enabled=0');send(b'ASEL\x00','APPS_EXIT')
 send(b'LRUN','LISTEN_HOLD 0');send(b'TLIV','TTS_DIAGNOSTIC');link.close();log.close()
s=(a.output/'serial.log').read_text(errors='replace');events=[{'size':int(size),'missing':int(missing),'text':bytes.fromhex(text).decode()} for size,missing,text in re.findall(r'FONT_TEXT size=(\d+) missing=(\d+) utf8=([0-9a-f]*)',s)]
for text in (expected_date,expected_time,'じこくがみせってい'):assert any(e['text']==text and e['missing']==0 for e in events),(text,events)
assert 'abort()' not in s and 'Guru Meditation' not in s
r['rendered_clock']=[e for e in events if e['text'] in (expected_date,expected_time,'じこくがみせってい')];(a.output/'result.json').write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n');print(json.dumps({k:v for k,v in r.items() if k not in ('boot','after','rendered_clock')},ensure_ascii=False))

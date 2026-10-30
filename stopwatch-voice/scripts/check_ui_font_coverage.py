#!/usr/bin/env python3
"""Check actual VLW Unicode tables and every FONT_TEXT draw event, without ink heuristics."""
import argparse,hashlib,json,re,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
DYNAMIC={17:'0123456789%:/.',20:'ラップリセットとめるはじめる',24:'0123456789:.+-/Hzcent',32:'0123456789.Hz',40:'0123456789:.',56:'0123456789'}
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--log',type=Path);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
fonts={};tables=[]
for path in sorted((ROOT/'assets').glob('ui*.vlw')):
 data=path.read_bytes();count,_,size,_,_,_=struct.unpack_from('>6i',data)
 codes=[struct.unpack_from('>I',data,24+i*28)[0] for i in range(count)]
 assert codes==sorted(set(codes)),path
 assert len(data)==24+28*count+sum(struct.unpack_from('>II',data,28+i*28)[0]*struct.unpack_from('>II',data,28+i*28)[1] for i in range(count)),path
 fonts[size]=set(codes)
 tables.append({'size':size,'sha256':hashlib.sha256(data).hexdigest(),'codepoints':[f'U+{c:04X}' for c in codes]})
checks=[]
def check(size,text,source,device_missing=None):
 missing=[f'{c} U+{ord(c):04X}' for c in sorted(set(text)) if c!=' ' and ord(c) not in fonts[size]]
 assert device_missing is None or device_missing==sum(c!=' ' and ord(c) not in fonts[size] for c in text),(size,text,'device/table disagreement')
 checks.append(dict(size=size,text=text,source=source,missing=missing))
for size,text in DYNAMIC.items():check(size,text,'dynamic format alphabet')
# Extract fixed labels from the renderer; variable values have explicit alphabets
# and exhaustive note/choice vocabularies below. Runtime events check final formatting.
source=(ROOT/'main/voice_screen.cpp').read_text()
app_source=source
for encoded,size in re.findall(r'text_at\("((?:[^"\\]|\\.)*)",\s*(\d+)',app_source):
 check(int(size),json.loads('"'+encoded+'"'),'fixed renderer label')
def literals(fragment):
 return [json.loads('"'+v+'"') for v in re.findall(r'"((?:[^"\\]|\\.)*)"',fragment)]
notes=source[source.index('static constexpr const char *ja[]'):source.index('const bool changed=full')]
for text in literals(notes):check(32,text,'all tuner note names')
menu=source[source.index('constexpr const char *app_names'):source.index('constexpr int app_radius')]
for text in literals(menu):check(17,text,'all menu labels')
ear=(ROOT/'main/ear_game.cpp').read_text()
ear=ear[ear.index('constexpr std::array<const char*, 12>'):ear.index('std::vector<int16_t> renderQuestionAudio')]
for text in literals(ear):
 for size in (15,17):check(size,text,'all ear choices and answer vocabulary')
apps=(ROOT/'main/apps.cpp').read_text()
labels=apps[apps.index('const auto &laps=stopwatch.laps()'):apps.index('voice_screen::app_clock(date')]
for text in literals(labels):
 if '%' not in text:
  check(17,text,'timer details and clock fields')
  check(24,text,'timer controls and clock fields')
check(15,'メモリがたりません','allocation failure menu message')
for text in ['せいかい','ちがうよ']:check(24,text,'ear feedback')
radio=(ROOT/'main/radio_app.cpp').read_text()
for text in re.findall(r'return "([^"]+)";',radio):check(15 if text=='メモリがたりません' else 24,text,'wireless status or failed-entry menu label')
if a.log:
 events=re.findall(r'FONT_TEXT size=(\d+) missing=(\d+) utf8=([0-9a-f]*)',a.log.read_text(errors='replace'))
 assert events,'no draw events captured'
 for size,missing,encoded in events:check(int(size),bytes.fromhex(encoded).decode(), 'device draw',int(missing))
 failures=[c for c in checks if c['missing']]
else:failures=[c for c in checks if c['missing']]
result={'space_rule':'U+0020 is an advance-only special case in pinned M5GFX; no VLW bitmap required','tables':tables,'checks':checks,'missing_checks':len(failures),'draw_events':sum(c['source']=='device draw' for c in checks)}
a.output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n')
for failure in failures:print(json.dumps(failure,ensure_ascii=False))
assert not failures,f'{len(failures)} displayed strings/alphabets contain missing glyphs'
print(f"PASS font Unicode tables: {len(checks)} strings/alphabets, {result['draw_events']} actual draws, zero missing glyphs")

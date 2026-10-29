#!/usr/bin/env python3
"""Record real LSTA memory/fps for BRIEF.md's >=30 minute on-device animation soak.

Always wrap this command in device_lock.py --wait 3600. No host audio playback.
uv run --with pyserial scripts/check_characters_soak.py --port /dev/cu.usbmodem1101 --output DIR
"""
import argparse
import json
import re
import time
from pathlib import Path
import serial


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port",required=True)
    parser.add_argument("--output",required=True,type=Path)
    parser.add_argument("--seconds",type=int,default=30*60)
    args=parser.parse_args()
    assert args.seconds>=30*60, "acceptance condition 6 requires at least 30 minutes"
    args.output.mkdir(parents=True,exist_ok=True)
    records,frames,errors=[],[],[]
    with serial.Serial(args.port,115200,timeout=1) as link, (args.output/'serial.log').open('w',buffering=1) as log:
        for command,expected in [(b'INFO','MEM serving '),(b'TMUT','TTS_DIAGNOSTIC muted=1 pcm=1')]:
            link.write(command)
            deadline=time.monotonic()+15  # same INFO/mute deadline as check_action.py
            while True:
                line=link.readline().decode(errors='replace').rstrip()
                log.write(f'preflight {line}\n')
                if (command==b'INFO' and line.startswith(expected)) or line==expected: break
                if time.monotonic()>deadline: raise TimeoutError('boot/mute not confirmed; soak not started')
        start=time.monotonic()
        next_stat=start
        while len(records)<2 or records[-1]['elapsed']-records[0]['elapsed']<args.seconds:
            now=time.monotonic()
            if now-start>args.seconds+2*60: raise TimeoutError('30-minute LSTA sample window incomplete')
            if now>=next_stat:
                link.write(b'LSTA')
                next_stat=now+60  # existing firmware's memory reporting interval
            line=link.readline().decode(errors='replace').rstrip()
            if not line:
                if now-start>args.seconds+60: raise TimeoutError('final LSTA memory record missing')
                continue
            log.write(f'{time.monotonic()-start:.3f} {line}\n')
            if line.startswith('MEM listening '):
                record={'elapsed':time.monotonic()-start,**{k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)}}
                records.append(record)
                print(json.dumps(record),flush=True)
            elif line.startswith('CHARACTER_STAT '):
                frames.append({'elapsed':time.monotonic()-start,**{k:float(v) for k,v in re.findall(r'(\w+)=([\d.]+)',line)}})
            elif line.startswith(('ERROR','Guru Meditation','BOOT reset_reason')):
                errors.append(line)
        duration=time.monotonic()-start
    deltas={k:records[-1][k]-records[0][k] for k in ('internal_free','psram_free')}
    span=records[-1]['elapsed']-records[0]['elapsed']
    result={'elapsed_seconds':duration,'sample_span_seconds':span,'records':records,'render_stats':frames,'errors':errors,'end_minus_start':deltas,
            'memory_not_decreased':all(v>=0 for v in deltas.values()),'full_duration':span>=30*60}
    (args.output/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k not in ('records','render_stats')}),flush=True)
    if errors or not result['memory_not_decreased']: raise SystemExit(1)


if __name__=='__main__': main()

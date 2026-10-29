#!/usr/bin/env python3
"""Compare real board startup detection. Invoke only through device_lock.py.

The app can be an older build; the current shared partition table is always kept.
No model or sprite data is written and no audio is played deliberately.
"""
import argparse
import hashlib
import json
import re
import subprocess
import time
from pathlib import Path

import serial


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--app', type=Path, required=True)
    p.add_argument('--partition-table', type=Path, required=True)
    p.add_argument('--port', required=True)
    p.add_argument('--count', type=int, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--tts', action='store_true')
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    tool = ['uvx', '--from', 'esptool==5.6.0', 'esptool', '--chip', 'esp32s3', '--port', args.port]
    rows = []
    for index in range(args.count):
        logpath = args.output / f'{index + 1:02d}-reset.log'
        with logpath.open('wb') as resetlog:
            if index == 0:
                command = tool + ['write-flash', '0x8000', str(args.partition_table), '0x10000', str(args.app)]
            else:
                command = tool + ['read-flash', '0x8000', '0xc00', str(args.output / f'{index + 1:02d}-table.bin')]
            subprocess.run(command, stdout=resetlog, stderr=subprocess.STDOUT, check=True)
        # Use the same existing 60-second observation limit as the device checks.
        deadline = time.monotonic() + 60
        with serial.Serial(args.port, 115200, timeout=0.5) as link, (args.output / f'{index + 1:02d}-boot.log').open('wb') as log:
            if args.tts:
                link.write(b'TMUT')
            row = {'trial': index + 1, 'autodetect': None, 'board': None, 'ready': False, 'tts_ready': None}
            started = time.monotonic()
            while time.monotonic() < deadline:
                raw = link.readline()
                if not raw:
                    continue
                log.write(raw)
                log.flush()
                line = raw.decode(errors='replace').strip()
                match = re.search(r'\[Autodetect\] (board_\w+)', line)
                if match:
                    row['autodetect'] = match[1]
                    if match[1] != 'board_M5StopWatch':
                        break
                match = re.search(r'BOARD id=(\d+) stopwatch=(\d+)', line)
                if match:
                    row['board'] = int(match[1])
                    row['stopwatch'] = match[2] == '1'
                    if args.tts:
                        link.write(b'TMUT')
                    if not row['stopwatch']:
                        break
                if line.startswith('TTS_INIT '):
                    row['tts_ready'] = 'ready=1' in line
                if 'READY action mode' in line or line.startswith('UI_STATE action_listening'):
                    row['ready'] = True
                    break
            row['seconds'] = round(time.monotonic() - started, 3)
            row['misdetected'] = row['autodetect'] not in (None, 'board_M5StopWatch') or row.get('stopwatch') is False
            row['classification'] = 'wrong_board' if row['misdetected'] else 'ready' if row['ready'] else 'startup_timeout'
            rows.append(row)
            print(json.dumps(row), flush=True)
        if index:
            assert (args.output / f'{index + 1:02d}-table.bin').read_bytes() == args.partition_table.read_bytes()
    summary = {'app': str(args.app), 'app_sha256': hashlib.sha256(args.app.read_bytes()).hexdigest(),
               'trials': rows, 'misdetected': sum(r['misdetected'] for r in rows),
               'startup_timeouts': sum(r['classification'] == 'startup_timeout' for r in rows),
               'ready': sum(r['ready'] for r in rows)}
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2))
    print(json.dumps({k: v for k, v in summary.items() if k != 'trials'}), flush=True)


if __name__ == '__main__':
    main()

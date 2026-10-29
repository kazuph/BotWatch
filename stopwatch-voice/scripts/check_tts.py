#!/usr/bin/env python3
"""Compare every device TTS PCM in a serial log with the actual host sanoTTS core.
No playback. A differing waveform is reported numerically, never labelled bit-identical.
"""
import argparse
import json
import re
import subprocess
from pathlib import Path
import numpy as np


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--log', required=True, type=Path)
    p.add_argument('--reference', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    model = Path(__file__).resolve().parents[1] / 'models/saanotts-jp-v4-int8.bin'
    rows, active, paused = [], None, None
    errors = []
    for line in args.log.read_text(errors='replace').splitlines():
        if line.startswith('ERROR TTS'):
            errors.append(line)
        if line.startswith('TTS_LISTEN state=paused'):
            paused = int(re.search(r'written=(\d+)', line)[1])
        if line.startswith('TTS_BEGIN '):
            if active:
                raise AssertionError('unterminated TTS response')
            assert line.endswith(' muted=1'), 'PCM verification requires confirmed device mute: ' + line
            caption = line.split('caption=', 1)[1].split(' kana=', 1)[0]
            active = {'caption': caption, 'pcm': bytearray(), 'chunks': 0, 'paused': paused}
        elif line.startswith('TTS_PCM '):
            assert active is not None
            m = re.fullmatch(r'TTS_PCM seq=(\d+) n=(\d+) ([0-9a-f]+)', line)
            assert m is not None and int(m[1]) == active['chunks'], line[:80]
            data = bytes.fromhex(m[3])
            assert len(data) == int(m[2]) * 2
            active['pcm'].extend(data)
            active['chunks'] += 1
        elif line.startswith('TTS_NORMALIZE '):
            assert active is not None
            active['normalization'] = dict(v.split('=', 1) for v in line.split()[1:])
        elif line.startswith('TTS_END '):
            assert active is not None
            fields = dict(v.split('=', 1) for v in line.split()[1:])
            assert fields['status'] == '0' and fields['audio_ok'] == '1', line
            assert len(active['pcm']) == int(fields['samples']) * 2 > 0
            n = len(rows) + 1
            dev = args.output / f'{n:03d}-device.pcm'
            ref = args.output / f'{n:03d}-reference.pcm'
            dev.write_bytes(active.pop('pcm'))
            run = subprocess.run([str(args.reference.resolve()), str(model), active['caption'], str(ref)], capture_output=True, text=True, check=True)
            x = np.frombuffer(dev.read_bytes(), '<i2').astype(np.float64)
            y = np.frombuffer(ref.read_bytes(), '<i2').astype(np.float64)
            measured_peak = int(np.max(np.abs(x)))
            measured_rms = float(np.sqrt(np.mean(x*x)))
            assert abs(measured_rms-float(fields['rms'])) < 0.000001
            assert fields['clips'] == '0', line
            if 'normalization' in active:
                assert measured_peak == int(fields['peak'])
                assert measured_peak <= round(32767*10**(-1/20))
                assert float(fields['gain']) > 0
            row = {**active, **fields, 'device_pcm': str(dev), 'reference_pcm': str(ref), 'host': run.stdout.strip(),
                   'length_equal': len(x) == len(y), 'device_samples': len(x), 'reference_samples': len(y)}
            row.update(measured_peak=measured_peak, measured_rms=measured_rms,
                       peak_dbfs=20*np.log10(measured_peak/32767) if measured_peak else None)
            row['sample_count_difference'] = len(x)-len(y)
            if min(len(x), len(y)) > 0:
                common = min(len(x), len(y))
                delta = x[:common]-y[:common]
                noise = float(np.sum(delta**2))
                row.update(bit_equal=bool(np.array_equal(x,y)), compared_prefix_samples=common, max_abs_diff=float(np.max(abs(delta))),
                           rmse=float(np.sqrt(np.mean(delta**2))),
                           snr_db=10 * np.log10(float(np.sum(y[:common]*y[:common]))/noise) if noise else None)
            rows.append(row)
            active = None
        elif line.startswith('TTS_LISTEN state=restored'):
            assert rows
            f = dict(v.split('=',1) for v in line.split()[1:])
            rows[-1]['listen_restored'] = f['ok'] == '1'
            rows[-1]['no_mic_samples_during_speech'] = f['before'] == f['after']
    assert active is None and rows, 'missing/unfinished TTS PCM'
    out = {'responses': rows, 'errors': errors,
           'all_lengths_equal': all(r['length_equal'] for r in rows),
           'all_restored': all(r.get('listen_restored', False) for r in rows),
           'all_paused': all(r.get('no_mic_samples_during_speech', False) for r in rows)}
    (args.output / 'summary.json').write_text(json.dumps(out, ensure_ascii=False, indent=2))
    print(json.dumps({k:v for k,v in out.items() if k!='responses'}, ensure_ascii=False))
    print('responses=', len(rows))
    if errors or not out['all_restored'] or not out['all_paused']:
        raise SystemExit(1)

if __name__ == '__main__':
    main()

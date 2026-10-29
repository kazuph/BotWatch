#!/usr/bin/env python3
"""Measure every frozen phrase with the actual host core and add its playback gain.
Run after generate_tts_phrases.py and build_tts_reference.py; rebuild the reference after this.
"""
import argparse,hashlib,json,re,struct,subprocess
from pathlib import Path


def main():
    root=Path(__file__).resolve().parents[1]
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference',type=Path,required=True)
    parser.add_argument('--report',type=Path,required=True)
    args=parser.parse_args()
    header=root/'main/tts_phrases.h';source=header.read_text()
    pattern=r'^    \{("(?:[^"\\]|\\.)*"), ("(?:[^"\\]|\\.)*"), [^}]+\},$'
    entries=re.findall(pattern,source,re.M);assert entries
    pcm_dir=args.report.parent/'tts-level-raw';pcm_dir.mkdir(parents=True,exist_ok=True)
    f32=lambda value:struct.unpack('<f',struct.pack('<f',value))[0]
    target=f32(10**(-1/20));results=[];gains={}
    for index,(caption_json,kana_json) in enumerate(entries):
        caption=json.loads(caption_json);path=pcm_dir/f'{index:03d}.pcm'
        run=subprocess.run([str(args.reference.resolve()),str(root/'models/saanotts-jp-v4-int8.bin'),caption,str(path),'--raw'],capture_output=True,text=True,check=True)
        fields=dict(re.findall(r'(\w+)=([^\s]+)',run.stdout));peak=float(fields['float_peak'])
        assert fields['status']=='0' and peak>0 and int(fields['samples'])>0
        gain=f32(target/peak);gains[caption]=gain
        results.append({'caption':caption,'kana':json.loads(kana_json),'gain':gain,'raw_float_peak':peak,
                        'raw_pcm_peak':int(fields['pcm_peak']),'raw_rms':float(fields['rms']),
                        'raw_clips':int(fields['clips']),'samples':int(fields['samples']),
                        'raw_pcm':str(path.resolve()),'raw_sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
    assert len(gains)==len(entries)
    def replace(match):
        caption=json.loads(match[1]);return '    {'+match[1]+', '+match[2]+f', {gains[caption]:.9f}f'+ '},'
    header.write_text(re.sub(pattern,replace,source,flags=re.M))
    args.report.write_text(json.dumps({'target_dbfs':-1,'target_peak':target,'phrases':results},ensure_ascii=False,indent=2)+'\n')
    print(f'PASS measured={len(results)} target_dbfs=-1 gain table generated',flush=True)


if __name__=='__main__':main()

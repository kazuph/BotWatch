#!/usr/bin/env python3
"""Audit tracked content and paths before creating a new public repository.
Private terms stay in ignored local files; license originals are checked separately.
"""
import argparse,hashlib,json,re,subprocess
from pathlib import Path


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[2]
    paths=subprocess.check_output(['git','ls-files','-z'],cwd=root).decode().split('\0')
    private_header=root/'stopwatch-voice/main/presets_local.h'
    terms=set()
    if private_header.exists():
        for text in re.findall(r'"([^"]+)"',private_header.read_text()):
            terms.add(text);terms.update(text.split())
    terms.update(p.parent.name for p in (root/'stopwatch-voice/pets').glob('*/pet.json'))
    patterns={'private_name_or_pet': '|'.join(re.escape(t) for t in sorted(terms)) or r'(?!)',
              'private_home':re.escape(str(Path.home())),
              'email':r'[A-Za-z0-9_.+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}',
              'ble_mac':r'(?i)(?:[0-9a-f]{2}:){5}[0-9a-f]{2}',
              'secret_assignment':r'''(?i)(?:password|wifi_ssid|wifi_pass|api_key|secret)\s*[:=]\s*["'][^"']+'''}
    hits=[];excluded=[];license_private_hits=[]
    for name in paths:
        if not name:continue
        path=root/name;data=path.read_bytes();text=data.decode(errors='ignore')
        license_original='licenses' in path.parts or path.name.startswith('THIRD_PARTY_NOTICES') or path.name=='NOTICE'
        if license_original:
            original=subprocess.check_output(['git','show','HEAD:'+name],cwd=root)
            assert original==data,('license original changed',name)
            for kind in ['private_name_or_pet','private_home']:
                if re.search(patterns[kind],text):license_private_hits.append({'file':name,'kind':kind})
            excluded.append({'file':name,'sha256':hashlib.sha256(data).hexdigest(),
                             'reason':'unaltered upstream copyright/license; public author emails retained',
                             'upstream_emails':len(re.findall(patterns['email'],text))})
            continue
        for kind,pattern in patterns.items():
            count=len(re.findall(pattern,text+'\n'+name))
            if count:hits.append({'file':name,'kind':kind,'count':count})
    report={'tracked_files':len([p for p in paths if p]),'patterns':patterns,'hits':hits,
            'excluded_licenses':excluded,'excluded_license_private_hits':license_private_hits,
            'private_local_files_included':False,'status':'PASS' if not hits and not license_private_hits else 'FAIL'}
    args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
    print(json.dumps({'status':report['status'],'hits':len(hits),'excluded_license_private_hits':len(license_private_hits),'excluded_licenses':len(excluded)}))
    if hits or license_private_hits:raise SystemExit(1)


if __name__=='__main__':main()

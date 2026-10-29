#!/usr/bin/env python3
"""Fetch the pinned v4 sanoTTS int8 model; verify release SHA256SUMS before use."""
import hashlib
from pathlib import Path
import urllib.request

NAME = 'saanotts-jp-v4-int8.bin'
SIZE = 654032
SHA256 = 'a1eb6b0812e2ad2a228836088a3e34160cb66731492fb957a5891605db2fa1b6'

def main():
    target = Path(__file__).resolve().parents[1] / 'models' / NAME
    def valid(path):
        return path.is_file() and path.stat().st_size == SIZE and hashlib.sha256(path.read_bytes()).hexdigest() == SHA256
    if not valid(target):
        temporary = target.with_suffix('.download')
        try:
            with urllib.request.urlopen('https://github.com/ayutaz/sanoTTS-jp/releases/download/v1.0.0/' + NAME, timeout=120) as response:
                temporary.write_bytes(response.read())
            if not valid(temporary):
                raise RuntimeError('sanoTTS model size or checksum mismatch')
            temporary.replace(target)
        finally:
            temporary.unlink(missing_ok=True)
    print(f'TTS_MODEL PASS size={SIZE} sha256={SHA256}')

if __name__ == '__main__':
    main()

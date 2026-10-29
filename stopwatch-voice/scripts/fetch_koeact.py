#!/usr/bin/env python3
"""Download the KoeAct-JP v0.1.1 CoreS3 files (Ralomi e13 stream40 .espdl and decoder weights).

Sizes and SHA256 come from the release's SHA256SUMS; a mismatch fails and keeps nothing.
"""
import hashlib
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
REVISION = "v0.1.1"
FILES = {
    "ralomi_e13_stream40.espdl": (4813760, "dce593c8c9293c5565d1126e2f5b96a8e8151259044fc0d3c2f79a80d62eca92"),
    "a2_weights.bin": (1132544, "61ee72e27af12907dd535908ee941b29fdee2bc3521e421705812adfa05ba4a6"),
}


def verify(path, size, sha256):
    return path.stat().st_size == size and hashlib.sha256(path.read_bytes()).hexdigest() == sha256


def main():
    for name, (size, sha256) in FILES.items():
        target = ROOT / "models" / name
        target.parent.mkdir(exist_ok=True)
        if not target.exists() or not verify(target, size, sha256):
            url = f"https://huggingface.co/ayousanz/KoeAct-JP/resolve/{REVISION}/cores3/{name}?download=true"
            temporary = target.with_suffix(".download")
            try:
                with urllib.request.urlopen(url, timeout=120) as response, temporary.open("wb") as output:
                    while block := response.read(65536):
                        output.write(block)
                if not verify(temporary, size, sha256):
                    raise RuntimeError(f"{name}: size or SHA256 differs from KoeAct-JP {REVISION} SHA256SUMS")
                temporary.replace(target)
            finally:
                temporary.unlink(missing_ok=True)
        print(f"KOEACT PASS {name} bytes={size} sha256={sha256}")


if __name__ == "__main__":
    main()

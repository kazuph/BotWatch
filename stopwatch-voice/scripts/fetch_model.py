#!/usr/bin/env python3
"""Download the agreed model; reject LFS pointers and corrupt downloads."""
import hashlib
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
NAME = "ralomi_w16a16_stream100.espdl"
SIZE = 4334768
SHA256 = "80db75b2ed89e74948eeac0259919ea5a49cd684d5f14c7c94d5aee5a0c4323c"
URL = f"https://huggingface.co/ayousanz/ralomi/resolve/main/esp32s3/{NAME}?download=true"


def verify(path):
    return path.stat().st_size == SIZE and hashlib.sha256(path.read_bytes()).hexdigest() == SHA256


def main():
    target = ROOT / "models" / NAME
    target.parent.mkdir(exist_ok=True)
    if not target.exists() or not verify(target):
        temporary = target.with_suffix(".download")
        try:
            with urllib.request.urlopen(URL, timeout=120) as response, temporary.open("wb") as output:
                while block := response.read(65536):
                    output.write(block)
            if not verify(temporary):
                raise RuntimeError("Model size or SHA256 differs from UPSTREAM_SHA256SUMS")
            temporary.replace(target)
        finally:
            temporary.unlink(missing_ok=True)
    print(f"MODEL PASS bytes={target.stat().st_size} sha256={SHA256} path={target}")


if __name__ == "__main__":
    main()

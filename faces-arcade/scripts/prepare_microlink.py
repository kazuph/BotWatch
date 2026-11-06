#!/usr/bin/env python3
"""Fetch the pinned MicroLink source and apply the task's TCP patch."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DEST = ROOT / ".dependencies/microlink"
PATCH = ROOT / "patches/microlink-tcp-deadline.patch"
COMMIT = "216da3300f0493b0860247d43f7af5ce29df63a5"


def run(*args, cwd=None, check=True, input_data=None):
    return subprocess.run(args, cwd=cwd, check=check, input=input_data, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def source_line_endings(patch):
    headers = (b"diff ", b"index ", b"--- ", b"+++ ", b"@@")
    return b"".join(line if line.startswith(headers) else line[:-1] + b"\r\n"
                    for line in patch.splitlines(keepends=True))


def main():
    if not DEST.exists():
        DEST.parent.mkdir(parents=True, exist_ok=True)
        run("git", "clone", "--no-checkout", "https://github.com/CamM2325/microlink.git", str(DEST))
        run("git", "checkout", "--detach", COMMIT, cwd=DEST)
    if run("git", "rev-parse", "HEAD", cwd=DEST).stdout.decode().strip() != COMMIT:
        raise RuntimeError("MicroLink source is not at the pinned commit")
    diff = run("git", "diff", "--unified=0", "--binary", "--", "components/microlink/src/ml_tcp.c", cwd=DEST).stdout.replace(b"\r\n", b"\n")
    expected = PATCH.read_bytes()
    if not diff:
        patch_data = source_line_endings(expected)
        run("git", "apply", "--unidiff-zero", "--check", "-", cwd=DEST, input_data=patch_data)
        run("git", "apply", "--unidiff-zero", "-", cwd=DEST, input_data=patch_data)
        diff = run("git", "diff", "--unified=0", "--binary", "--", "components/microlink/src/ml_tcp.c", cwd=DEST).stdout.replace(b"\r\n", b"\n")
    if diff != expected:
        raise RuntimeError("MicroLink source does not match the approved patch")
    status = run("git", "status", "--porcelain", "--untracked-files=all", cwd=DEST).stdout.decode().splitlines()
    if status != [" M components/microlink/src/ml_tcp.c"]:
        raise RuntimeError("MicroLink source contains unexpected changes")
    print("MicroLink pinned commit and TCP patch verified")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        if isinstance(exc, subprocess.CalledProcessError) and exc.stderr:
            print(exc.stderr.decode(errors="replace"), file=sys.stderr)
        print(str(exc), file=sys.stderr)
        sys.exit(1)

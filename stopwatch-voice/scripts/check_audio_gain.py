#!/usr/bin/env python3
"""Compile the actual C++ processor and check exact bytes on saved real PCM."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--study", type=Path, required=True)
parser.add_argument("--capture", type=Path, help="Also check actual board raw-to-normalized bytes")
args = parser.parse_args()
study = json.loads(args.study.read_text())
sources = [Path(row["source"]) for row in study[0]["records"]]
code = r'''
#include "audio_gain.h"
#include <cstdio>
#include <vector>
#include <cassert>
int main() {
    std::vector<int16_t> pcm;
    int16_t sample;
    while (std::fread(&sample, sizeof(sample), 1, stdin)) { pcm.push_back(sample); }
    std::vector<int16_t> batch = pcm;
    audio_gain::normalize(batch.data(), batch.size(), 100 * 160);
    std::vector<int16_t> streamed(pcm.size());
    audio_gain::Stream stream;
    for (int end = 50 * 160; end <= (int)pcm.size(); end += 50 * 160) {
        stream.append(pcm.data(), end, streamed.data(), pcm.size(), 100 * 160);
    }
    assert(streamed == batch);
    uint64_t energy = 0;
    for (const int sample : batch) { energy += uint64_t(sample * sample); }
    const double expected_rms = 10 * std::log10(double(energy) / batch.size() / (32768.0 * 32768.0) + 1e-12);
    assert(stream.rms_dbfs() == expected_rms);
    assert(!stream.quiet_speech(nullptr, 0, 160, 4));
    pcm = streamed;
    std::fwrite(pcm.data(), sizeof(int16_t), pcm.size(), stdout);
}
'''


def expected(data):
    raw = [value for (value,) in struct.iter_unpack("<h", data)]
    leading = next((i for i, value in enumerate(raw) if value), len(raw))
    raw = raw[leading:] + [0] * leading
    result = []
    for first in range(0, len(raw), 100 * 160):
        part = raw[first:first + 100 * 160]
        scale = min(16, -32752 / min(-1, min(part)), 32751 / max(1, max(part)))
        result.extend(int(value * scale) for value in part)
    return struct.pack(f"<{len(result)}h", *result)


with tempfile.TemporaryDirectory() as directory:
    folder = Path(directory)
    (folder / "check.cpp").write_text(code)
    executable = folder / "check"
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "main"), str(folder / "check.cpp"), "-o", str(executable)], check=True)
    cases = [(str(path), path.read_bytes()) for path in sources]
    if args.capture:
        cases.append(("actual-board-capture", (args.capture / "raw-microphone.pcm").read_bytes()))
    cases += [("all-zero", bytes(64000 * 2)), ("leading-zero", bytes(16000 * 2) + sources[-1].read_bytes()[:48000 * 2]),
              ("int16-rails", struct.pack("<4h", -32768, 32767, 0, 1) * 16000)]
    results = []
    for name, data in cases:
        actual = subprocess.run([str(executable)], input=data, capture_output=True, check=True).stdout
        assert actual == expected(data), name
        if name == "actual-board-capture":
            assert actual == (args.capture / "microphone.pcm").read_bytes(), "board/host processing mismatch"
        assert len(actual) == len(data), name
        samples = list(struct.iter_unpack("<h", actual))
        assert all(-32752 <= value <= 32751 for (value,) in samples), name
        results.append({"case": name, "stream_equals_batch": True, "exact_cpp_python_bytes": True, "samples": len(samples), "within_rails": True})
    output = args.study.parent / "cpp-normalization-check.json"
    output.write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results, indent=2))

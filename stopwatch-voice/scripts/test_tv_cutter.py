#!/usr/bin/env python3
"""Exercise the firmware's utterance cutter without opening a device or sending IR."""
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / "main/main.cpp").read_text()
constants = source[source.index("constexpr int kHopsPerSecond"):source.index("// run_koeact() keeps at most")]
start = source.index("struct Cutter {")
cutter = source[start:source.index("\n\nvoid serve_action(", start)]
program = '''#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include "features.h"
''' + constants + '\nconstexpr int kListenSamples = ralomi::kSampleRate * 4;\n' + cutter + '''
int main() {
    for (int minimum : {kMinVoicedHops, kOnsetHops}) {
        Cutter cutter;
        Cutter::Event event = Cutter::Event::none;
        for (int i = 0; i < 75; ++i) {
            const int64_t hop = i * ralomi::kHop;
            const auto next = cutter.step(i < 5 ? -20 : -90, hop, hop + ralomi::kHop, minimum);
            if (next != Cutter::Event::none) event = next;
        }
        assert(event == (minimum == kOnsetHops ? Cutter::Event::segment : Cutter::Event::dropped));
    }
    std::puts("PASS 50ms utterance: idle drops, TV command wait recognizes");
}
'''
with tempfile.TemporaryDirectory() as folder:
    test = Path(folder) / "test.cpp"
    binary = Path(folder) / "test"
    test.write_text(program)
    subprocess.run(["c++", "-std=c++17", "-I", str(root / "main"), str(test), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

#!/usr/bin/env python3
import json
from pathlib import Path
import sys
from check_imagenet_labels import main as check

check()
root = Path(__file__).resolve().parents[1]
labels = json.loads((root / 'assets/imagenet_ja.json').read_text(encoding='utf-8'))
out = Path(sys.argv[1])
out.parent.mkdir(parents=True, exist_ok=True)
strings = ',\n'.join('    ' + json.dumps(label['ja'], ensure_ascii=False) for label in labels)
out.write_text('#pragma once\nstatic const char* const kImagenetJa[] = {\n' + strings + '\n};\n', encoding='utf-8')

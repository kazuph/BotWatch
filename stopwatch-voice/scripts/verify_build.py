#!/usr/bin/env python3
"""Check actual IDF output before flashing (uv run --with pyyaml)."""
import argparse
import csv
import hashlib
import json
from pathlib import Path

import yaml

from fetch_model import NAME, SHA256, SIZE
from fetch_tts import SHA256 as TTS_SHA256, SIZE as TTS_SIZE


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--app-only", action="store_true", help="validate writes of only partition table and app; preserve all data partitions")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = root / "build"
    config = json.loads((build / "config/sdkconfig.json").read_text())
    assert config["IDF_TARGET"] == "esp32s3"
    assert config["SPIRAM_MODE_OCT"] and config["ESPTOOLPY_FLASHSIZE_16MB"]
    ble = {key: config[key] for key in (
        "BT_ENABLED", "BT_NIMBLE_ENABLED", "BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL",
        "BT_NIMBLE_MAX_CONNECTIONS", "BT_CTRL_BLE_MAX_ACT")}
    assert ble == {"BT_ENABLED": True, "BT_NIMBLE_ENABLED": True,
                   "BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL": True,
                   "BT_NIMBLE_MAX_CONNECTIONS": 1, "BT_CTRL_BLE_MAX_ACT": 2}, "stage 2 NimBLE configuration differs"
    dependencies = yaml.safe_load((root / "dependencies.lock").read_text())["dependencies"]
    versions = {name: item["version"] for name, item in dependencies.items()}
    for name, version in {
        "m5stack/m5unified": "0.2.27", "m5stack/m5gfx": "0.2.34",
        "espressif/esp-dl": "3.3.13", "espressif/esp-dsp": "1.8.2", "idf": "5.5.5",
    }.items():
        assert versions[name] == version, f"dependency mismatch: {name}"
    flash = json.loads((build / "flasher_args.json").read_text())
    flash_size = int(flash["flash_settings"]["flash_size"].removesuffix("MB")) * 1024 * 1024
    partitions = {}
    for row in csv.reader((root / "partitions.csv").read_text().splitlines()):
        if not row or row[0].lstrip().startswith("#"):
            continue
        name, _, _, offset, size, *_ = [value.strip() for value in row]
        partitions[name] = {"offset": int(offset, 0), "size": int(size, 0)}
        assert int(offset, 0) + int(size, 0) <= flash_size
    files = []
    for offset, filename in flash["flash_files"].items():
        data = (build / filename).read_bytes()
        address = int(offset, 0)
        assert address + len(data) <= flash_size, f"write outside flash: {filename}"
        files.append({"offset": address, "length": len(data), "end_exclusive": address + len(data), "file": filename, "sha256": hashlib.sha256(data).hexdigest()})
    files.sort(key=lambda item: item["offset"])
    for left, right in zip(files, files[1:]):
        assert left["end_exclusive"] <= right["offset"], "overlapping writes"
    app = next(item for item in files if item["file"] == flash["app"]["file"])
    model = next(item for item in files if Path(item["file"]).name == NAME)
    assert app["offset"] == partitions["factory"]["offset"]
    assert app["length"] <= partitions["factory"]["size"], "app exceeds factory"
    assert model["offset"] == partitions["model"]["offset"]
    assert model["length"] == SIZE and model["sha256"] == SHA256
    assert model["length"] <= partitions["model"]["size"]
    # BRIEF.md: retain all model locations, reserve D00000..DFFFFF for the other task.
    for name, offset, size in (("model", 0x300000, 0x440000), ("koeact", 0x740000, 0x4A0000),
                               ("a2", 0xBE0000, 0x120000), ("tts", 0xD00000, 0x100000),
                               ("sprites", 0xE00000, 0x200000)):
        assert partitions[name] == {"offset": offset, "size": size}, f"partition contract changed: {name}"
    tts = next(item for item in files if item["offset"] == partitions["tts"]["offset"])
    assert tts["length"] == TTS_SIZE and tts["sha256"] == TTS_SHA256
    writes = [item for item in files if item["offset"] in (0x8000, app["offset"])] if args.app_only else files
    assert all(item["end_exclusive"] <= 0xD00000 or item["offset"] >= 0xE00000 for item in writes), "write into reserved speech region"
    sprite_files = [item for item in files if item["offset"] == partitions["sprites"]["offset"]]
    manifest_path = root / "assets/pets/manifest.json"
    pet_count = json.loads(manifest_path.read_text())["count"] if manifest_path.exists() else 0
    assert 0 <= pet_count <= 3
    if pet_count:
        assert len(sprite_files) == 1
        sprites = sprite_files[0]
        assert sprites["length"] <= partitions["sprites"]["size"]
        assert sprites["sha256"] == hashlib.sha256((root / "assets/pets/sprites.bin").read_bytes()).hexdigest()
    else:
        assert not sprite_files, "zero-pet build must not flash the sprite partition"
    evidence = {"status": "PASS", "versions": versions, "octal_psram": True, "ble_configuration": ble, "flash_size": flash_size, "partitions": partitions, "files": files, "write_files": writes, "app_free_bytes": partitions["factory"]["size"] - app["length"]}
    args.output.write_text(json.dumps(evidence, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps(evidence, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()

"""Standalone Ralomi ONNX inference; no training repository is required.

Copyright 2026 ayousanz. Licensed under the Apache License, Version 2.0.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path

import numpy as np
import onnxruntime as ort
import torch
from features import LogMelFeatureExtractor


def prepare_audio(sample_rate: int, samples: np.ndarray) -> np.ndarray:
    """Normalize PCM and resample a bounded mono/stereo input to 16kHz."""
    if not isinstance(sample_rate, (int, np.integer)) or not 8000 <= sample_rate <= 192000:
        raise ValueError("8〜192kHzの音声を指定してください。")
    samples = np.asarray(samples)
    if samples.ndim not in (1, 2) or (samples.ndim == 2 and samples.shape[1] not in (1, 2)):
        raise ValueError("モノラルまたはステレオの音声を指定してください。")
    if not 0.1 <= len(samples) / sample_rate <= 15:
        raise ValueError("音声の長さは0.1〜15秒にしてください。")
    if np.issubdtype(samples.dtype, np.signedinteger):
        scale = float(max(abs(np.iinfo(samples.dtype).min), np.iinfo(samples.dtype).max))
        samples = samples.astype(np.float32) / scale
    elif samples.dtype == np.uint8:
        samples = (samples.astype(np.float32) - 128) / 128
    elif np.issubdtype(samples.dtype, np.floating):
        samples = samples.astype(np.float32)
    else:
        raise ValueError("対応していない音声の形式です。")
    if not np.isfinite(samples).all():
        raise ValueError("音声に無効な数値が含まれています。")
    if samples.ndim == 2:
        samples = samples.mean(axis=1, dtype=np.float32)
    samples = np.clip(samples, -1, 1)
    if sample_rate != 16000:
        from scipy.signal import resample_poly

        divisor = math.gcd(int(sample_rate), 16000)
        samples = resample_poly(samples, 16000 // divisor, int(sample_rate) // divisor)
    return np.ascontiguousarray(samples[:240000], dtype=np.float32)


def decode(logits: np.ndarray, length: int, tokens: list[str]) -> str:
    """Match ja-mora-v0 greedy CTC, including blank-separated repeats."""
    if logits.ndim != 3 or logits.shape[0] != 1 or logits.shape[2] != len(tokens):
        raise ValueError("model output shape differs from the vocabulary")
    if not 0 < length <= logits.shape[1] or not np.isfinite(logits).all():
        raise ValueError("model output is invalid")
    previous = None
    reading = []
    for token_id in logits[0, :length].argmax(axis=-1):
        index = int(token_id)
        if index != previous and index not in (0, 1):
            reading.append(tokens[index])
        previous = index
    return "".join(reading)


class RalomiRecognizer:
    """Load only the fixed inference artifacts and verify their checksums."""

    def __init__(self, model_directory: str | Path, *, threads: int = 2):
        root = Path(model_directory)
        metadata = json.loads((root / "release_metadata.json").read_text(encoding="utf-8"))
        for name in ("model.onnx", "inference_config.json", "vocabulary.json"):
            path = root / name
            expected = metadata["files"][name]
            if path.stat().st_size != expected["bytes"]:
                raise ValueError(f"model artifact size differs: {name}")
            if hashlib.sha256(path.read_bytes()).hexdigest() != expected["SHA256"]:
                raise ValueError(f"model artifact checksum differs: {name}")
        config = json.loads((root / "inference_config.json").read_text(encoding="utf-8"))
        vocabulary = json.loads((root / "vocabulary.json").read_text(encoding="utf-8"))
        self.tokens = vocabulary["tokens"]
        if self.tokens[:2] != ["<blank>", "<unk>"]:
            raise ValueError("unsupported CTC vocabulary")
        if config["model"]["family"] != "streaming_dfsmn_ctc":
            raise ValueError("unsupported acoustic model")
        features = config["features"]
        if features["sample_rate_hz"] != 16000 or features["subsampling_factor"] != 4:
            raise ValueError("unsupported feature geometry")
        if not isinstance(threads, int) or not 1 <= threads <= 16:
            raise ValueError("CPU threads must be between1 and16")
        self.extractor = LogMelFeatureExtractor(
            sample_rate_hz=16000,
            window_ms=features["window_ms"],
            hop_ms=features["hop_ms"],
            fft_size=features["fft_size"],
            mel_bins=features["mel_bins"],
            normalization=features.get("normalization", "none"),
        ).eval()
        options = ort.SessionOptions()
        options.intra_op_num_threads = threads
        options.inter_op_num_threads = 1
        self.session = ort.InferenceSession(
            str(root / "model.onnx"), options, providers=["CPUExecutionProvider"]
        )

    def transcribe(self, sample_rate: int, samples: np.ndarray) -> str:
        audio = prepare_audio(sample_rate, samples)
        waveforms = torch.from_numpy(audio.copy()).unsqueeze(0)
        with torch.no_grad():
            features, lengths = self.extractor(waveforms, torch.tensor([len(audio)]))
        length = int(lengths[0])
        if not 4 <= length <= 1500:
            raise ValueError("音声の長さを確認してください。")
        padded = np.zeros((1, 1500, 40), dtype=np.float32)
        padded[0, :length] = features[0].numpy()
        logits = self.session.run(None, {"log_mel_features": padded})[0]
        return decode(logits, length // 4, self.tokens)


def main() -> None:
    import soundfile as sf

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("audio", type=Path)
    parser.add_argument("--model-directory", type=Path, default=Path(__file__).parent)
    args = parser.parse_args()
    samples, sample_rate = sf.read(args.audio, dtype="float32")
    print(RalomiRecognizer(args.model_directory).transcribe(sample_rate, samples))


if __name__ == "__main__":
    main()

"""Causal log-Mel feature extraction shared by training and reference inference."""

from __future__ import annotations

import math
from dataclasses import dataclass

import torch
from torch import Tensor, nn
from torch.nn import functional as F


@dataclass(frozen=True)
class FeatureStreamState:
    """Bounded waveform context retained between streaming chunks."""

    samples: Tensor


def _hz_to_mel(frequency_hz: Tensor) -> Tensor:
    return 2595.0 * torch.log10(1.0 + frequency_hz / 700.0)


def _mel_to_hz(mel: Tensor) -> Tensor:
    return 700.0 * (torch.pow(10.0, mel / 2595.0) - 1.0)


def build_mel_filterbank(
    *,
    sample_rate_hz: int,
    fft_size: int,
    mel_bins: int,
    minimum_hz: float = 20.0,
    maximum_hz: float | None = None,
) -> Tensor:
    """Build a deterministic HTK triangular mel filterbank."""
    upper_hz = float(maximum_hz or sample_rate_hz / 2)
    if not 0 <= minimum_hz < upper_hz <= sample_rate_hz / 2:
        raise ValueError("mel frequency bounds must lie within the Nyquist interval")
    if fft_size <= 0 or mel_bins <= 0:
        raise ValueError("fft_size and mel_bins must be positive")

    mel_edges = torch.linspace(
        _hz_to_mel(torch.tensor(minimum_hz, dtype=torch.float64)),
        _hz_to_mel(torch.tensor(upper_hz, dtype=torch.float64)),
        mel_bins + 2,
        dtype=torch.float64,
    )
    hz_edges = _mel_to_hz(mel_edges)
    frequencies = torch.linspace(
        0.0,
        sample_rate_hz / 2,
        fft_size // 2 + 1,
        dtype=torch.float64,
    )
    lower = hz_edges[:-2, None]
    center = hz_edges[1:-1, None]
    upper = hz_edges[2:, None]
    rising = (frequencies - lower) / (center - lower)
    falling = (upper - frequencies) / (upper - center)
    return torch.clamp(torch.minimum(rising, falling), min=0.0).to(torch.float32)


class LogMelFeatureExtractor(nn.Module):
    """Generate 40-bin log-Mel frames without future padding or centering."""

    def __init__(
        self,
        *,
        sample_rate_hz: int = 16_000,
        window_ms: float = 25.0,
        hop_ms: float = 10.0,
        fft_size: int = 512,
        mel_bins: int = 40,
        log_floor: float = 1e-10,
        normalization: str = "none",
        normalization_epsilon: float = 1e-5,
    ) -> None:
        super().__init__()
        self.sample_rate_hz = sample_rate_hz
        self.window_samples = round(sample_rate_hz * window_ms / 1000)
        self.hop_samples = round(sample_rate_hz * hop_ms / 1000)
        self.fft_size = fft_size
        self.mel_bins = mel_bins
        self.log_floor = log_floor
        self.normalization = normalization
        self.normalization_epsilon = normalization_epsilon
        if self.window_samples != 400 or self.hop_samples != 160:
            raise ValueError("v0 requires a 400-sample window and 160-sample hop")
        if fft_size < self.window_samples:
            raise ValueError("fft_size must be at least the window length")
        if mel_bins != 40:
            raise ValueError("v0 requires 40 mel bins")
        if not math.isfinite(log_floor) or log_floor <= 0:
            raise ValueError("log_floor must be finite and positive")
        if normalization not in {"none", "frame-mean", "frame-standardize"}:
            raise ValueError("normalization must be none, frame-mean, or frame-standardize")
        if not math.isfinite(normalization_epsilon) or normalization_epsilon <= 0:
            raise ValueError("normalization_epsilon must be finite and positive")

        self.register_buffer("window", torch.hann_window(self.window_samples, periodic=True))
        self.register_buffer(
            "mel_filterbank",
            build_mel_filterbank(
                sample_rate_hz=sample_rate_hz,
                fft_size=fft_size,
                mel_bins=mel_bins,
            ),
        )

    def frame_count(self, sample_lengths: Tensor) -> Tensor:
        """Return the number of complete, non-padded frames per waveform."""
        return torch.clamp(
            torch.div(
                sample_lengths - self.window_samples,
                self.hop_samples,
                rounding_mode="floor",
            )
            + 1,
            min=0,
        )

    def forward(self, waveforms: Tensor, sample_lengths: Tensor) -> tuple[Tensor, Tensor]:
        """Extract features from a zero-padded waveform batch."""
        if waveforms.ndim != 2:
            raise ValueError("waveforms must have shape [batch, samples]")
        if sample_lengths.ndim != 1 or sample_lengths.shape[0] != waveforms.shape[0]:
            raise ValueError("sample_lengths must have shape [batch]")
        if waveforms.shape[1] < self.window_samples:
            empty = waveforms.new_empty((waveforms.shape[0], 0, self.mel_bins))
            return empty, self.frame_count(sample_lengths)

        frames = waveforms.unfold(1, self.window_samples, self.hop_samples)
        windowed = frames * self.window.to(dtype=waveforms.dtype)
        padded = F.pad(windowed, (0, self.fft_size - self.window_samples))
        spectrum = torch.fft.rfft(padded, n=self.fft_size, dim=-1)
        power = spectrum.real.square() + spectrum.imag.square()
        mel_energies = torch.matmul(
            power,
            self.mel_filterbank.to(dtype=power.dtype).transpose(0, 1),
        )
        features = torch.log(torch.clamp(mel_energies, min=self.log_floor))
        if self.normalization != "none":
            features = features - features.mean(dim=-1, keepdim=True)
            if self.normalization == "frame-standardize":
                variance = features.square().mean(dim=-1, keepdim=True)
                features = features * torch.rsqrt(variance + self.normalization_epsilon)
        return features, self.frame_count(sample_lengths)

    def initial_stream_state(
        self,
        batch_size: int,
        *,
        device: torch.device | str | None = None,
        dtype: torch.dtype = torch.float32,
    ) -> FeatureStreamState:
        """Create an empty state whose retained context is always below one window."""
        return FeatureStreamState(torch.empty(batch_size, 0, device=device, dtype=dtype))

    def stream(
        self,
        waveform_chunk: Tensor,
        state: FeatureStreamState,
    ) -> tuple[Tensor, FeatureStreamState]:
        """Emit every complete frame and retain only bounded overlap/context."""
        if waveform_chunk.ndim != 2 or state.samples.ndim != 2:
            raise ValueError("streaming waveforms and state must have shape [batch, samples]")
        if waveform_chunk.shape[0] != state.samples.shape[0]:
            raise ValueError("streaming batch size cannot change")
        samples = torch.cat((state.samples, waveform_chunk), dim=1)
        if samples.shape[1] < self.window_samples:
            return (
                samples.new_empty((samples.shape[0], 0, self.mel_bins)),
                FeatureStreamState(samples),
            )

        frame_count = 1 + (samples.shape[1] - self.window_samples) // self.hop_samples
        consumed = frame_count * self.hop_samples
        lengths = torch.full(
            (samples.shape[0],), samples.shape[1], device=samples.device, dtype=torch.long
        )
        features, _ = self(samples, lengths)
        next_state = FeatureStreamState(samples[:, consumed:])
        if next_state.samples.shape[1] >= self.window_samples:
            raise RuntimeError("feature streaming state exceeded its fixed capacity")
        return features, next_state

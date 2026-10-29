#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <cmath>

namespace audio_gain {
// M5Unified Mic_Class.inl uses these rails; magnification defaults to 16.
constexpr int kLow = std::numeric_limits<int16_t>::min() + 16;
constexpr int kHigh = std::numeric_limits<int16_t>::max() - 16;
constexpr double kMaximumGain = 16;
// study_volume_warning.py: midpoint of the closest labeled saved-PCM groups.
constexpr double kQuietNormalizedDbfs = (-35.4643599478439 - 26.572500092672996) / 2;

inline double normalize_chunk(int16_t *pcm, int samples)
{
    int low = -1, high = 1;
    for (int i = 0; i < samples; ++i) {
        low = std::min(low, (int)pcm[i]);
        high = std::max(high, (int)pcm[i]);
    }
    const double gain = std::min(kMaximumGain, std::min((double)kLow / low, (double)kHigh / high));
    if (gain == kMaximumGain) {
        // This branch is exactly the same integer-valued 16x operation, without
        // a software double multiply for every sample on the ESP32-S3.
        for (int i = 0; i < samples; ++i) { pcm[i] = static_cast<int16_t>((int)pcm[i] * (int)kMaximumGain); }
    } else {
        const bool negative_peak = (double)kLow / low <= (double)kHigh / high;
        const int numerator = negative_peak ? -kLow : kHigh;
        const int denominator = negative_peak ? -low : high;
        for (int i = 0; i < samples; ++i) {
            const int product = (int)pcm[i] * numerator;  // fits signed int32
            // Nonintegral fractions are at least 1/32768 from an integer,
            // much farther than double rounding. Retain double only at exact
            // integer boundaries, so even its truncation behavior is unchanged.
            pcm[i] = product % denominator == 0
                ? static_cast<int16_t>(pcm[i] * gain)
                : static_cast<int16_t>(product / denominator);
        }
    }
    return gain;
}

inline int normalize(int16_t *pcm, int samples, int chunk_samples)
{
    int leading = 0;
    while (leading < samples && pcm[leading] == 0) { ++leading; }
    std::memmove(pcm, pcm + leading, (samples - leading) * sizeof(int16_t));
    std::fill(pcm + samples - leading, pcm + samples, 0);
    for (int first = 0; first < samples; first += chunk_samples) {
        const int end = std::min(samples, first + chunk_samples);
        normalize_chunk(pcm + first, end - first);
    }
    return leading;
}

class Stream {
public:
    int append(const int16_t *raw, int end, int16_t *output, int samples, int chunk_samples)
    {
        for (int i = m_received; i < end; ++i) {
            if (m_copied == 0 && raw[i] == 0) { ++m_leading; }
            else { output[m_copied++] = raw[i]; }
        }
        m_received = end;
        if (end == samples) {
            std::fill(output + m_copied, output + samples, 0);
            m_copied = samples;
        }
        while (m_copied - m_normalized >= chunk_samples ||
               (end == samples && m_normalized < samples)) {
            const int count = std::min(chunk_samples, m_copied - m_normalized);
            const double gain = normalize_chunk(output + m_normalized, count);
            m_chunk_samples = chunk_samples;
            if (gain == kMaximumGain) { m_capped_chunks |= uint32_t{1} << (m_normalized / chunk_samples); }
            for (int i = 0; i < count; ++i) {
                const int sample = output[m_normalized + i];
                m_energy += uint64_t(sample * sample);
            }
            m_normalized += count;
        }
        return m_normalized;
    }
    int leading() const { return m_leading; }
    double rms_dbfs() const
    {
        return 10 * std::log10((m_normalized ? double(m_energy) / m_normalized / (32768.0 * 32768.0) : 0) + 1e-12);
    }
    bool quiet_speech(const int *tokens, size_t count, int hop, int subsampling) const
    {
        if (!m_chunk_samples) { return false; }
        uint32_t speech_chunks = 0;
        for (size_t step = 0; step < count; ++step) {
            if (tokens[step] > 1) { speech_chunks |= uint32_t{1} << (step * hop * subsampling / m_chunk_samples); }
        }
        return speech_chunks && (speech_chunks & m_capped_chunks) == speech_chunks && rms_dbfs() < kQuietNormalizedDbfs;
    }
private:
    int m_received = 0, m_copied = 0, m_normalized = 0, m_leading = 0;
    int m_chunk_samples = 0;
    uint32_t m_capped_chunks = 0;
    uint64_t m_energy = 0;
};
}  // namespace audio_gain

// Run the unmodified sanoTTS C99 core on the host with the firmware's phrase composition.
#include "tts_text.h"
#include "tts_level.h"
extern "C" {
#include "saanotts.h"
#include "saanotts_stream.h"
#include "g2p.h"
#include "saan_pcm.h"
}
#include <cstdio>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
int main(int argc, char **argv) {
    if (argc != 4 && !(argc==5 && !std::strcmp(argv[4],"--raw"))) { fprintf(stderr, "usage: tts_reference model.bin caption output.pcm [--raw]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { return 2; }
    fseek(f, 0, SEEK_END); const long size = ftell(f); rewind(f);
    std::vector<unsigned char> blob(size); if (fread(blob.data(), 1, size, f) != size_t(size)) { return 2; } fclose(f);
    saan_weights w; if (saan_weights_open(&w, blob.data(), blob.size()) != SAAN_OK) { return 3; }
    const bool raw=argc==5;float gain=0;
    std::vector<char> kana(tts::kana_capacity());
    if (!tts::intermediate(argv[2], kana.data(), kana.size(),&gain) || (!raw && !(gain>0))) { fprintf(stderr, "unsupported caption: %s\n", argv[2]); return 4; }
    std::vector<int32_t> ids(saan_g2p_capacity(kana.size())); int32_t count;
    if (saan_g2p(kana.data(), strlen(kana.data()), ids.data(), ids.size(), &count, nullptr) != SAAN_G2P_OK) { return 5; }
    std::vector<unsigned char> workspace(saan_stream_arena_peak(count) + 16);
    void *aligned = reinterpret_cast<void *>((reinterpret_cast<uintptr_t>(workspace.data()) + 15) & ~uintptr_t(15));
    saan_arena a; saan_arena_init(&a, aligned, saan_stream_arena_peak(count));
    saan_stream st; auto status = saan_stream_init(&st, &w, &a, ids.data(), count, SAAN_S_V);
    if (status != SAAN_OK) { fprintf(stderr, "init %d\n", status); return 6; }
    f = fopen(argv[3], "wb"); if (!f) { return 7; }
    saan_pcm_reset();float absolute_peak=0;
    tts_level::Loudness loudness(SAAN_SR);
    while (true) {
        const float *pcm; int32_t n;
        status = saan_stream_pull_ptr(&st, &pcm, &n);
        n *= SAAN_HOP;
        if (status != SAAN_OK || !n) { break; }
        for (int i = 0; i < n; ++i) {
            absolute_peak=std::max(absolute_peak,std::abs(pcm[i]));
            const uint16_t q = saan_f32_to_i16(raw?pcm[i]:loudness.process(pcm[i]*gain));
            fputc(q & 255, f); fputc(q >> 8, f);
        }
    }
    fclose(f);
    printf("status=%d ids=%d samples=%u arena_peak=%zu kana=%s fnv=%016llx float_peak=%.9g pcm_peak=%d rms=%.6f clips=%u gain=%.9g\n", status, count, (unsigned)saan_pcm_samples(), a.peak, kana.data(), (unsigned long long)saan_pcm_checksum(),absolute_peak,saan_pcm_absmax(),
        std::sqrt(double(saan_pcm_sqsum())/saan_pcm_samples()),(unsigned)saan_pcm_clip_count(),gain);
    return status == SAAN_OK ? 0 : 8;
}

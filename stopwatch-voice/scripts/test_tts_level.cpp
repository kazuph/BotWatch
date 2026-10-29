#include "tts_level.h"
#include "tts_text.h"
#include "tts_phrases.h"
extern "C" {
#include "saan_pcm.h"
}
#include <cassert>
#include <cstdio>
#include <cmath>
#include <vector>
#include <cstring>

int main()
{
    assert(tts_level::gain(0)==1);
    std::vector<char> kana(tts::kana_capacity());
    for(const auto &phrase:kTtsPhrases) {
        float gain=0;
        assert(phrase.gain>0 && std::isfinite(phrase.gain));
        assert(tts::intermediate(phrase.caption,kana.data(),kana.size(),&gain));
        assert(gain==phrase.gain);
    }
    auto coefficient=[](const char *caption) {
        for(const auto &phrase:kTtsPhrases) if(!std::strcmp(phrase.caption,caption)) return phrase.gain;
        assert(false);return 0.0f;
    };
    float composed=0;
    assert(tts::intermediate("おとのおおきさ 50",kana.data(),kana.size(),&composed));
    assert(composed==std::min({coefficient("おとのおおきさ "),coefficient("5"),coefficient("十")}));
    assert(tts::intermediate("26℃にしたよ",kana.data(),kana.size(),&composed));
    assert(composed==std::min({coefficient("2"),coefficient("十"),coefficient("6"),coefficient("℃にしたよ")}));
    for(float peak:{0.0001f,0.01f,0.1f,0.9f,1.0f,2.0f}) {
        saan_pcm_reset();
        const float gain=tts_level::gain(peak);
        for(int i=-100;i<=100;++i) saan_f32_to_i16(peak*i/100*gain);
        assert(saan_pcm_clip_count()==0);
        assert(std::abs(saan_pcm_absmax()-std::lround(tts_level::kTargetPeak*32767))<=1);
    }
    saan_pcm_reset();
    for(float sample:{-100.0f,-1.0f,0.0f,1.0f,100.0f})
        saan_f32_to_i16(tts_level::limit(sample));
    assert(saan_pcm_clip_count()==0);
    {
        // Loudness never exceeds the normalized peak, removes DC and raises a quiet tone.
        tts_level::Loudness loudness(22050);
        saan_pcm_reset();
        double in_energy=0,out_energy=0,dc=0;
        for(int i=0;i<22050;++i) {
            const float x=(i<11025?0.2f:0.9f)*std::sin(2*M_PI*1000*i/22050.0)+0.3f;
            const float y=loudness.process(x);
            assert(std::abs(y)<=tts_level::kTargetPeak);
            saan_f32_to_i16(y);
            if(i>=2205 && i<11025) { in_energy+=double(x-0.3f)*(x-0.3f); out_energy+=double(y)*y; dc+=y; }
        }
        assert(saan_pcm_clip_count()==0);
        assert(std::abs(dc/8820)<0.01);
        assert(10*std::log10(out_energy/in_energy)>6);
    }
    const auto dac=tts_level::dac_volume(4,true);assert(dac==0xcd);
    const auto peak=tts_level::kTargetPeak*tts_level::maximum_i2s_gain(4,true)*std::pow(10.0,(int(dac)-0xbf)/40.0);
    assert(peak<1 && peak*std::pow(10.0,0.5/20)>1);
    std::printf("PASS normalized_peak=%.6f clips=0 dac=0x%x worst_output_peak=%.6f next_step_clips=1\n",tts_level::kTargetPeak,dac,peak);
}

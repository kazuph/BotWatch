#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <span>
#include "audio_buffer.h"
namespace audio_spectrum {
// Twofold zero padding makes log-parabolic Blackman-Harris peak interpolation accurate
// on the 2048-sample tuner window; verified at both calibrated keyboard ends.
constexpr size_t padding=2;
// ESP-DSP 1.8.2 dsps_wind_blackman_harris_f32 coefficients.
constexpr double a0=0.35875,a1=0.48829,a2=0.14128,a3=0.01168;
struct Peak { double hz=0, amplitude=0; };
class Workspace {
    audio_memory::Buffer<std::byte> scratch;
    audio_memory::Buffer<float> coefficients,result;
    size_t sampleCount=0,fftCount=0;
public:
    static constexpr size_t requiredBytes(size_t count) {
        size_t n=1;while(n<count*padding)n*=2;
        return n*sizeof(std::complex<float>)+(count+n/2+1)*sizeof(float);
    }
    bool begin(size_t count) noexcept {
        end();size_t n=1;while(n<count*padding)n*=2;
        if(!scratch.allocate(n*sizeof(std::complex<float>)) || !coefficients.allocate(count) || !result.allocate(n/2+1)){end();return false;}
        sampleCount=count;fftCount=n;
        for(size_t i=0;i<count;++i){const double phase=2*M_PI*i/(count-1);coefficients[i]=a0-a1*std::cos(phase)+a2*std::cos(2*phase)-a3*std::cos(3*phase);}
        return true;
    }
    void end() noexcept {scratch.reset();coefficients.reset();result.reset();sampleCount=fftCount=0;}
    size_t bytes() const noexcept {return scratch.bytes()+coefficients.bytes()+result.bytes();}
    size_t scratchBytes() const noexcept {return scratch.bytes();}
    size_t bins() const noexcept {return result.size();}
    void *scratchData() noexcept {return scratch.data();}
    bool ready() const noexcept {return fftCount!=0;}
    std::span<const float> power(std::span<const int16_t> samples) noexcept {
        if(!ready() || samples.size()!=sampleCount)return {};
        // YIN and FFT run sequentially. Start the FFT objects in the same PSRAM
        // scratch bytes after YIN's scalar confidence has been copied out.
        auto *a=static_cast<std::complex<float>*>(scratchData());
        std::uninitialized_value_construct_n(a,fftCount);
        const size_t n=fftCount;
        float mean=0;for(auto s:samples)mean+=s;mean/=samples.size();
        for(size_t i=0;i<samples.size();++i)a[i]=(samples[i]-mean)*coefficients[i];
        for(size_t i=1,j=0;i<n;++i){size_t bit=n>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;if(i<j)std::swap(a[i],a[j]);}
        for(size_t length=2;length<=n;length*=2){std::complex<float> step(std::cos(-2*M_PI/length),std::sin(-2*M_PI/length));
            for(size_t i=0;i<n;i+=length){std::complex<float>w=1;for(size_t j=0;j<length/2;++j){auto u=a[i+j],v=a[i+j+length/2]*w;a[i+j]=u+v;a[i+j+length/2]=u-v;w*=step;}}}
        for(size_t i=0;i<result.size();++i)result[i]=std::norm(a[i]);
        return {result.data(),result.size()};
    }
};
inline Peak peak(std::span<const float> p,size_t i,size_t sample_count,double sample_rate) {
    double shift=0;
    if(i>0 && i+1<p.size() && p[i-1]>0 && p[i]>0 && p[i+1]>0) {
        const double left=std::log(p[i-1]),mid=std::log(p[i]),right=std::log(p[i+1]);
        const double denominator=left-2*mid+right;
        if(denominator!=0)shift=0.5*(left-right)/denominator;
    }
    return {(i+shift)*sample_rate/(2*(p.size()-1)),2*std::sqrt(p[i])/(a0*sample_count)};
}
}

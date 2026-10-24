#pragma once
// Adapted from Patchlane Sources/AudioCore/DipoleDSP.hpp.
// Device geometry: 8 cm speaker spacing (iPad: 15 cm), 35 cm listening distance.
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <memory>
#include <vector>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
namespace sc55dipole {
constexpr double Pi=3.14159265358979323846;
using Z=std::complex<double>;
struct LCDipoleConfig {
    double spacingCM=8, distanceCM=35, headCM=17.5, maxBoostDB=12, trimDB=0;
    int tonalReference=1;
    double geqDB[31]{};
};
inline Z shadow(double theta,double radius,double f) {
    double alpha=1.05+0.95*std::cos(theta/(5*Pi/6)*Pi),wt=2*Pi*f*2*radius/343;
    double dt=theta<=Pi/2 ? -radius/343*std::cos(theta):radius/343*(theta-Pi/2);
    return Z(1,alpha*wt)/Z(1,wt)*std::polar(1.,-2*Pi*f*dt);
}
inline bool valid(const LCDipoleConfig &c) {
    if(!std::isfinite(c.spacingCM)||!std::isfinite(c.distanceCM)||!std::isfinite(c.headCM)||!std::isfinite(c.maxBoostDB)||!std::isfinite(c.trimDB))return false;
    if(c.spacingCM<2||c.spacingCM>200||c.distanceCM<20||c.distanceCM>500||c.headCM<10||c.headCM>25||c.maxBoostDB<0||c.maxBoostDB>24||c.trimDB<-12||c.trimDB>12||c.tonalReference<0||c.tonalReference>2||std::atan2(c.spacingCM/2,c.distanceCM)>Pi/4)return false;
    for(double db:c.geqDB)if(!std::isfinite(db)||db<-12||db>12)return false;
    return true;
}
inline std::array<Z,2> response(const LCDipoleConfig &c,double f,double rate,bool includeGEQ=true) {
    double radius=c.headCM/200,x=c.spacingCM/200,z=c.distanceCM/100,angle=std::atan2(x,z),r=std::hypot(x,z);
    auto front=shadow(Pi/2,radius,f);
    auto a=shadow(Pi/2-angle,radius,f)/front*(r/std::hypot(x-radius,z));
    auto b=shadow(Pi/2+angle,radius,f)/front*(r/std::hypot(x+radius,z));
    double maxGain=std::pow(10,c.maxBoostDB/20),lambda=1/(4*maxGain*maxGain);
    auto inv=[&](Z h){return std::conj(h)/(std::norm(h)+lambda);};
    Z mid=inv(a+b),side=inv(a-b);
    if(c.tonalReference==1) { double scale=1/std::max(std::abs(mid),1e-9);mid*=scale;side*=scale; }
    if(c.tonalReference==2) { double scale=1/std::max(std::sqrt((std::norm(mid)+std::norm(side))/2),1e-9);mid*=scale;side*=scale; }
    double peak=std::max(std::abs(mid),std::abs(side));
    if(peak>maxGain){mid*=maxGain/peak;side*=maxGain/peak;}
    if(f<300) {
        double keep=f<=100?1:0.5+0.5*std::cos(Pi*(f-100)/200),mp=std::arg(mid),sp=std::abs(side)>1e-9?std::arg(side):mp;
        double delta=std::atan2(std::sin(sp-mp),std::cos(sp-mp)),phase=f>=100?1:0.5-0.5*std::cos(Pi*f/100);
        mid=std::polar(std::pow(std::max(std::abs(mid),1e-9),1-keep),mp);
        side=std::polar(std::pow(std::max(std::abs(side),1e-9),1-keep),sp*(1-keep)+(mp+delta*phase)*keep);
    }
    double weight=f<=6000?1:f>=10000?0:0.5+0.5*std::cos(Pi*(f-6000)/4000);
    mid=mid*weight+Z(1-weight,0);side=side*weight+Z(1-weight,0);
    double eqWeight=c.tonalReference==1||f<=200?1:f>=500?0:0.5+0.5*std::cos(Pi*(f-200)/300);
    double eq=std::pow(std::min(std::pow(10,24./20),1/std::max(std::abs(mid),1e-9)),eqWeight);
    Z geq=includeGEQ?std::pow(10,c.trimDB/20):1;
    constexpr double centres[31]={20,25,31.5,40,50,63,80,100,125,160,200,250,315,400,500,630,800,1000,1250,1600,2000,2500,3150,4000,5000,6300,8000,10000,12500,16000,20000};
    Z v=std::polar(1.,-2*Pi*f/rate);
    for(int i=0;i<31;++i)if(includeGEQ&&c.geqDB[i]!=0) {
        double A=std::pow(10,c.geqDB[i]/40),w=2*Pi*centres[i]/rate,alpha=std::sin(w)/(2*4.318),co=std::cos(w);
        geq*=((1+alpha*A)-2*co*v+(1-alpha*A)*v*v)/((1+alpha/A)-2*co*v+(1-alpha/A)*v*v);
    }
    return {mid*eq*geq,side*eq*geq};
}
// Coefficient synthesis runs only in prepareToPlay. Portable radix-2 FFT
// preserves Patchlane's homomorphic minimum-phase reconstruction.
inline void fft(std::vector<Z>& values, bool inverse) {
    const size_t n=values.size();
    for(size_t i=1,j=0;i<n;++i) {
        size_t bit=n>>1;
        for(;j&bit;bit>>=1)j^=bit;
        j^=bit;
        if(i<j)std::swap(values[i],values[j]);
    }
    for(size_t length=2;length<=n;length<<=1) {
        const auto root=std::polar(1.,(inverse?2:-2)*Pi/double(length));
        for(size_t base=0;base<n;base+=length) {
            Z phase=1;
            for(size_t j=0;j<length/2;++j) {
                const auto a=values[base+j], b=values[base+j+length/2]*phase;
                values[base+j]=a+b; values[base+j+length/2]=a-b;
                phase*=root;
            }
        }
    }
    if(inverse)for(auto& value:values)value/=double(n);
}
struct Coefficients {
    static constexpr int MaxTaps=1024, SynthesisSize=32768;
    double rate;
    std::array<std::array<float,MaxTaps>,2> reversed{};
    std::array<int,2> activeTaps{};
    explicit Coefficients(double sampleRate, double spacingCM=8):rate(sampleRate) {
        int taps=256;
        while(rate>48000.*(taps/256)&&taps<MaxTaps)taps*=2;
        LCDipoleConfig config;
        config.spacingCM=spacingCM;
        for(int mode=0;mode<2;++mode) {
            std::vector<Z> spectrum(SynthesisSize);
            for(int k=0;k<=SynthesisSize/2;++k) {
                const auto h=response(config,double(k)*rate/SynthesisSize,rate,false)[mode];
                spectrum[k]=std::log(std::max(std::abs(h),1e-12));
                if(k>0&&k<SynthesisSize/2)spectrum[SynthesisSize-k]=spectrum[k];
            }
            fft(spectrum,true);
            for(int k=1;k<SynthesisSize/2;++k)spectrum[k]=Z(2*spectrum[k].real(),0);
            spectrum[0]=Z(spectrum[0].real(),0);
            spectrum[SynthesisSize/2]=Z(spectrum[SynthesisSize/2].real(),0);
            std::fill(spectrum.begin()+SynthesisSize/2+1,spectrum.end(),Z{});
            fft(spectrum,false);
            for(int k=0;k<=SynthesisSize/2;++k) {
                spectrum[k]=std::exp(spectrum[k]);
                if(k==0||k==SynthesisSize/2)spectrum[k]=Z(spectrum[k].real(),0);
                else spectrum[SynthesisSize-k]=std::conj(spectrum[k]);
            }
            fft(spectrum,true);
            std::array<float,MaxTaps> impulse{};
            const int tail=taps/8;
            for(int k=0;k<taps;++k) {
                const double window=k<taps-tail?1.:.5+.5*std::cos(Pi*(k-(taps-tail))/(tail-1));
                const float value=float(spectrum[k].real()*window);
                impulse[k]=std::abs(value)<1e-12f?0.f:value;
            }
            int active=taps;
            while(active>1&&impulse[active-1]==0)--active;
            activeTaps[mode]=active;
            std::reverse_copy(impulse.begin(),impulse.begin()+active,reversed[mode].begin());
        }
    }
};
inline float dot(const float *samples,const float *kernel,int count) {
    float result=0;int k=0;
#if defined(__aarch64__)
    auto a=vdupq_n_f32(0),b=a,c=a,d=a;
    for(;k+16<=count;k+=16) {
        a=vfmaq_f32(a,vld1q_f32(samples+k),vld1q_f32(kernel+k));
        b=vfmaq_f32(b,vld1q_f32(samples+k+4),vld1q_f32(kernel+k+4));
        c=vfmaq_f32(c,vld1q_f32(samples+k+8),vld1q_f32(kernel+k+8));
        d=vfmaq_f32(d,vld1q_f32(samples+k+12),vld1q_f32(kernel+k+12));
    }
    a=vaddq_f32(vaddq_f32(a,b),vaddq_f32(c,d));
    for(;k+4<=count;k+=4)a=vfmaq_f32(a,vld1q_f32(samples+k),vld1q_f32(kernel+k));
    result=vaddvq_f32(a);
#endif
    for(;k<count;++k)result+=samples[k]*kernel[k];
    return result;
}
class Processor {
    std::unique_ptr<Coefficients> coefficients;
    alignas(64) std::array<std::array<float,Coefficients::MaxTaps*2>,2> history{};
    int index=0;
    float blend=0;
    bool dormant=true;
public:
    void prepare(double rate, double spacingCM=8) {
        coefficients=std::isfinite(rate)&&rate>0 ? std::make_unique<Coefficients>(rate,spacingCM):nullptr;
        for(auto& past:history)past.fill(0);
        index=0;blend=0;dormant=true;
    }
    void process(float* left,float* right,int frames,bool enabled) noexcept {
        if(frames<=0||!coefficients||(!enabled&&blend==0))return;
        if(enabled&&dormant) {
            for(auto& past:history)past.fill(0);
            index=0;dormant=false;
        }
        const float step=float(1/(coefficients->rate*.01));
        for(int f=0;f<frames;++f) {
            const float l=left[f],r=right[f],modal[2]={(l+r)*.5f,(l-r)*.5f};
            float filtered[2];
            for(int mode=0;mode<2;++mode) {
                auto& past=history[mode];
                past[index]=past[index+Coefficients::MaxTaps]=modal[mode];
                const int count=coefficients->activeTaps[mode];
                filtered[mode]=dot(past.data()+index+Coefficients::MaxTaps+1-count,
                                   coefficients->reversed[mode].data(),count);
            }
            blend=enabled?std::min(1.f,blend+step):std::max(0.f,blend-step);
            left[f]=l+(filtered[0]+filtered[1]-l)*blend;
            right[f]=r+(filtered[0]-filtered[1]-r)*blend;
            index=(index+1)&(Coefficients::MaxTaps-1);
        }
        if(!enabled&&blend==0)dormant=true;
    }
};
}

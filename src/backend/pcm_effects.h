// SC-55 のエフェクト（ERAM のリバーブ／コーラス）を浮動小数で書き直したもの。
//
// pcm.cpp の該当部は回路の転写で、20 ビットの値を 16 ビットの遅延メモリに詰めるための
// ブロック浮動小数（14 ビット仮数 + 2 ビット指数）と、シフトによる係数乗算でできている。
// 浮動小数にすると詰め替えは丸ごと不要になり、係数は実数の掛け算になる。
//
// ビルド設定に手を入れずに済むよう、実装もこのヘッダに置いている（.jucer に .cpp を
// 足し忘れると Xcode 側でリンクが通らないため）。
//
// 構成（PCM_SIMULATION.md に地図がある）:
//   段 1-4    シュレーダー型オールパスの縦続（拡散）
//   段 5-6    一次ローパス（減衰）
//   段 7      左右への振り分け
//   段 8-16   尾のコムと出力の足し込み
//   段 17-18  リバーブから出力バスへ
//   段 19-20  コーラス（LFO で変調されるタップ）
//   段 21-23,31  コーラスから出力バスへ
//
// 係数と固定タップは設定として保持し、コーラス読出し位置とリバーブの
// 振分けランプを毎サンプル受け取る。PCMレジスタ変換は互換アダプターの責務。
#pragma once

#include <cstdint>
#include <cstring>
#include <array>
#include "sc55_chorus_oscillator.h"
#include "sc55_envelope_ramp.h"
#include "sc55_chorus_setup.h"
#include "sc55_reverb_setup.h"
#include "sc55_effect_parameter.h"

// Control-rate settings and audio-rate modulation, without chip RAM.
struct PCMEffectsSettings
{
    struct Coefficient { float high=0,highHalf=0,low=0; bool feedback=false; };
    static Coefficient decode(uint16_t word) noexcept
    {
        const float high=float(int8_t(word>>8))*(1.0f/32.0f);
        return {high,high*0.5f,float(int8_t(word&255))*(1.0f/32.0f),bool(word&0x30)};
    }
    std::array<uint16_t,12> diffusionTaps{},tailTaps{};
    Coefficient reverbInput,chorusInput,comb;
    std::array<Coefficient,2> diffusion{},damping{},reverbReturn{};
    std::array<Coefficient,4> chorusReturn{};
};
struct PCMEffectsModulation
{
    uint16_t leftTap=0,rightTap=0;
    float leftFraction=0,rightFraction=0;
    float reverbSpread=0;
};

struct PCMEffects
{
    static constexpr int DELAY_WORDS = 0x4000;   // 16,384 語 = 32 kHz で 512 ms

    PCMEffectsSettings settings;
    sc55::ChorusOscillator chorus;
    sc55::EnvelopeRamp spread;
    uint16_t leftTap=0,rightTap=0;
    std::array<uint16_t,2> interpolation{};
    bool phaseMarker=false; // Retained by the legacy readback adapter.
    float delay[DELAY_WORDS] {};                 // 遅延メモリ。整数版の eram に対応
    float a[8] {};                               // ram1[28][0..7] に対応する作業値
    float b[8] {};                               // ram1[29][0..7] に対応する作業値

    void configureChorus(const sc55::ChorusSetup& setup) noexcept
    {
        chorus={setup.position,setup.begin,setup.end,0,setup.increment,false,true,false};
        phaseMarker=false;
        settings.tailTaps[9]=0x3800;
        for(unsigned i=0;i<4;++i) settings.chorusReturn[i]=PCMEffectsSettings::decode(setup.returns[i]);
        // Preserve delay/filter histories and current interpolation/taps. The
        // next audio frame advances those, just as on the existing path.
    }

    void configureReverb(const sc55::ReverbSetup& setup) noexcept
    {
        settings.diffusionTaps=setup.diffusionTaps;
        for(unsigned i=0;i<9;++i) settings.tailTaps[i]=setup.tailTaps[i];
        for(unsigned i=0;i<2;++i) {
            settings.diffusion[i]=PCMEffectsSettings::decode(setup.diffusion[i]);
            settings.damping[i]=PCMEffectsSettings::decode(setup.damping[i]);
            settings.reverbReturn[i]=PCMEffectsSettings::decode(setup.output);
        }
        settings.comb=PCMEffectsSettings::decode(setup.comb);
        spread.command=setup.spreadCommand;
        // Existing residual audio and the current spread level survive setup.
    }

    void update(sc55::EffectParameter parameter,uint16_t value) noexcept
    {
        using P=sc55::EffectParameter;
        const auto coefficient=PCMEffectsSettings::decode(value);
        switch(parameter) {
        case P::reverbInput: settings.reverbInput=coefficient; break;
        case P::reverbOutput: settings.reverbReturn.fill(coefficient); break;
        case P::reverbSpread: spread.command=value; break;
        case P::chorusInput: settings.chorusInput=coefficient; break;
        case P::chorusLevel:
            settings.chorusReturn[0]=coefficient;
            settings.chorusReturn[3]=PCMEffectsSettings::decode(value&0xff00); break;
        case P::chorusFeedback: settings.chorusReturn[1]=coefficient; break;
        case P::chorusSend: settings.chorusReturn[0]=coefficient; break;
        }
    }
    void setChorusMix(const std::array<uint8_t,3>& mix) noexcept
    {
        update(sc55::EffectParameter::chorusLevel,uint16_t((mix[0]<<8)|mix[2]));
        update(sc55::EffectParameter::chorusFeedback,mix[1]);
        settings.chorusReturn[2]={};
    }
    void beginReverbDrain() noexcept
    {
        spread.command=0xb6;
        settings.reverbInput={}; settings.reverbReturn={};
        settings.diffusion={}; settings.comb={}; settings.damping={};
        for(unsigned i=0;i<6;++i) settings.diffusionTaps[i]=uint16_t(i);
        settings.diffusionTaps[8]=6; settings.diffusionTaps[9]=7;
        settings.tailTaps[0]=0x2000; settings.tailTaps[1]=0x2001;
        settings.tailTaps[4]=0x2002; settings.tailTaps[5]=0x2003;
        // Draining is scheduled by the controller. Do not clear live history.
    }

    void reset() { std::memset(delay, 0, sizeof delay); std::memset(a, 0, sizeof a); std::memset(b, 0, sizeof b); }
    // Complete audio-owned effect frame: ramp, interpolation, delay network,
    // then advance the read heads. No register bridge is required by callers.
    void process(sc55::EnvelopeClock clock,bool active,float reverb,float chorusInput,
                 float dry[6],float sends[6]) noexcept;
};

// 20 ビットの範囲。整数版は sx20 で折り返すが、通常の動作では範囲に収まるので
// 浮動小数版では飽和させる（折り返しは音にならない）。
inline float PCMEffects_Clip (float v)
{
    constexpr float limit = 524288.0f;           // 2^19
    return v > limit - 1.0f ? limit - 1.0f : (v < -limit ? -limit : v);
}

inline void PCMEffects_Step (PCMEffects& fx, uint16_t tv, PCMEffectsModulation modulation,
                      float reverb_in, float chorus_in,
                      float rcadd[6], float rcadd2[6])
{
    const auto& settings=fx.settings;

    float* a = fx.a;
    float* b = fx.b;

    const auto rd  = [&] (uint16_t tap) { return fx.delay[(uint32_t) (tap + tv) & 0x3fff]; };
    const auto rd1 = [&] (uint16_t tap) { return rd(tap) * 0.5f; };            // type=1
    const auto rdo = [&] (uint16_t tap) { return fx.delay[(uint32_t) (tap + tv + 1) & 0x3fff]; };
    const auto wr  = [&] (uint16_t tap, float v) { fx.delay[(uint32_t) (tap + tv) & 0x3fff] = PCMEffects_Clip(v); };

    // 入力段。リバーブとコーラスのバスを一次ローパスに通してから拡散段へ渡す。
    b[1] = (b[1] * settings.chorusInput.high + chorus_in * settings.chorusInput.low) * 0.5f;
    b[0] = (b[0] * settings.reverbInput.high + reverb_in * settings.reverbInput.low) * 0.5f;

    // 段 1-4: シュレーダー型オールパスの縦続。(v1 & 0x30) が段の有効・無効。
    {   // 1
        const auto& v = settings.diffusion[0];
        const float s1 = rd1(settings.diffusionTaps[1]), s2 = rd(settings.diffusionTaps[1]);
        const float back = v.feedback ? s1 : 0.0f;
        const float t = b[0] * v.highHalf - back;
        b[4] = t;
        b[5] = t * v.low * 0.5f + s2;
    }
    {   // 2
        const auto& v = settings.diffusion[0];
        const float s1 = rd1(settings.diffusionTaps[2]), s2 = rd(settings.diffusionTaps[2]);
        const float back = v.feedback ? s1 : 0.0f;
        const float t = b[5] - back;
        b[5] = t;
        a[0] = t * v.low * 0.5f + s2;
    }
    {   // 3
        const auto& v = settings.diffusion[0];
        const float s1 = rd1(settings.diffusionTaps[3]), s2 = rd(settings.diffusionTaps[3]);
        const float back = v.feedback ? s1 : 0.0f;
        const float t = a[0] - back;
        a[0] = t;
        a[1] = t * v.low * 0.5f + s2;
        a[2] = rd(settings.diffusionTaps[5]);
    }
    {   // 4
        const auto& v = settings.diffusion[1];
        const float s1 = rd1(settings.diffusionTaps[4]), s2 = rd(settings.diffusionTaps[4]);
        const float back = v.feedback ? s1 : 0.0f;
        const float t = a[1] - back;
        a[1] = t;
        a[3] = t * v.low * 0.5f + s2;
        a[4] = rd(settings.tailTaps[1]);
    }

    // 段 5-6: 尾の減衰（一次ローパス）。ここで拡散段の出力を遅延メモリへ書き戻す。
    {   // 5
        const auto& v = settings.damping[0];
        b[2] = (b[2] * v.high + rd(settings.tailTaps[0]) * v.low) * 0.5f;
        wr(settings.diffusionTaps[0], b[4]);
    }
    {   // 6
        const auto& v = settings.damping[1];
        b[3] = (b[3] * v.high + rd(settings.tailTaps[8]) * v.low) * 0.5f;
        wr(settings.diffusionTaps[1], b[5]);
        wr(settings.diffusionTaps[2], a[0]);
    }
    {   // 7  左右に振り分ける
        const float in = a[3];
        a[3] = in + b[2] * modulation.reverbSpread * 0.5f;
        a[5] = in + b[3] * modulation.reverbSpread * 0.5f;
        wr(settings.diffusionTaps[3], a[1]);
    }

    // 段 8-16: 尾のコムと出力の足し込み。
    {   // 8
        const auto& v = settings.comb;
        const float t = a[3] + a[2] * v.high * 0.5f;
        a[3] = t;
        a[2] = a[2] + t * v.low * 0.5f;
        a[1] = rd(settings.diffusionTaps[9]);
    }
    {   // 9
        const auto& v = settings.comb;
        const float t = a[5] + a[4] * v.high * 0.5f;
        a[5] = t;
        a[4] = a[4] + t * v.low * 0.5f;
        b[4] = rd(settings.tailTaps[5]);
    }
    {   // 10
        const auto& v = settings.comb;
        const float in = a[1];
        const float t = in * v.high * 0.5f + rd(settings.diffusionTaps[8]);
        a[1] = t;
        b[5] = t * v.low * 0.5f + in;
        wr(settings.diffusionTaps[4], a[3]);
    }
    {   // 11
        const auto& v = settings.comb;
        const float in = b[4];
        const float t = in * v.high * 0.5f + rd(settings.tailTaps[4]);
        b[4] = t;
        a[0] = t * v.low * 0.5f + in;
        wr(settings.diffusionTaps[5], a[2]);
        wr(settings.tailTaps[0], a[5]);
    }
    a[5] = rd(settings.diffusionTaps[6]);                                     // 12
    {   // 13
        a[5] = a[5] + rd(settings.diffusionTaps[10]);
        a[2] = rd(settings.tailTaps[2]);
    }
    {   // 14
        a[5] = (rd(settings.tailTaps[6]) + a[2]) + a[5];
        a[2] = rd(settings.diffusionTaps[7]);
    }
    {   // 15
        a[2] = a[2] + rd(settings.diffusionTaps[11]);
        a[3] = rd(settings.tailTaps[3]);
    }
    {   // 16
        a[2] = (rd(settings.tailTaps[7]) + a[2]) + a[3];
        wr(settings.tailTaps[1], a[4]);
        wr(settings.diffusionTaps[8], a[1]);
    }

    // 段 17-18: リバーブから出力バスへ。ここで次の周回ぶんのコーラス入力も読む。
    {   // 17
        const auto& v = settings.reverbReturn[0];
        rcadd[0]  = a[5] * v.high;
        rcadd2[0] = a[5] * v.low;
        const float t = rdo(modulation.leftTap);
        wr(settings.diffusionTaps[9], b[5]);
        b[5] = t;
    }
    {   // 18
        const auto& v = settings.reverbReturn[1];
        rcadd[1]  = a[2] * v.high;
        rcadd2[1] = a[2] * v.low;
        a[1] = rdo(modulation.rightTap);
    }

    // 段 19-20: コーラス。タップは LFO で動かされ、係数もその位相から作られる。
    {   // 19
        const float fraction = modulation.leftFraction;
        const float s1 = rd(modulation.leftTap);
        wr(settings.tailTaps[4], b[4]);
        b[5] = (s1 - s1 * fraction * 0.5f) + b[5] * fraction * 0.5f;
    }
    {   // 20
        const float fraction = modulation.rightFraction;
        const float s1 = rd(modulation.rightTap);
        wr(settings.tailTaps[5], a[0]);
        a[1] = (s1 - s1 * fraction * 0.5f) + a[1] * fraction * 0.5f;
        wr(settings.tailTaps[9], b[1]);
    }

    // 段 21-23, 31: コーラスから出力バスへ。
    rcadd[2]  = b[5] * settings.chorusReturn[0].high;   rcadd2[2] = b[5] * settings.chorusReturn[0].low;
    rcadd[3]  = b[5] * settings.chorusReturn[1].high;   rcadd2[3] = b[5] * settings.chorusReturn[1].low;
    rcadd[4]  = a[1] * settings.chorusReturn[2].high;   rcadd2[4] = a[1] * settings.chorusReturn[2].low;
    rcadd[5]  = a[1] * settings.chorusReturn[3].high;   rcadd2[5] = a[1] * settings.chorusReturn[3].low;

    for (int i = 0; i < 6; ++i) { rcadd[i] = PCMEffects_Clip(rcadd[i]); rcadd2[i] = PCMEffects_Clip(rcadd2[i]); }
    for (int i = 0; i < 8; ++i) { a[i] = PCMEffects_Clip(a[i]); b[i] = PCMEffects_Clip(b[i]); }
}

inline void PCMEffects::process(sc55::EnvelopeClock clock,bool active,float reverb,
    float chorusInput,float dry[6],float sends[6]) noexcept
{
    const uint16_t phase=uint16_t(chorus.phase|(chorus.descending?0x8000:0)|(phaseMarker?0x4000:0));
    interpolation[(phase&0x8000)?0:1]=phase&0x7fff;
    const uint16_t complement=uint16_t(0x4000-phase);
    interpolation[(complement&0x8000)?1:0]=complement&0x7fff;
    spread.advance(sc55::EnvelopeRamp::Stage::secondGain,clock,active);
    const PCMEffectsModulation motion{leftTap,rightTap,
        float(int8_t(interpolation[0]>>8))*(1.0f/32.0f),
        float(int8_t(interpolation[1]>>8))*(1.0f/32.0f),
        float(int8_t(spread.level>>8))*(1.0f/32.0f)};
    PCMEffects_Step(*this,clock.phase,motion,reverb,chorusInput,dry,sends);
    chorus.advance(clock.update,active);
    leftTap=chorus.leftTap(); rightTap=chorus.rightTap();
}

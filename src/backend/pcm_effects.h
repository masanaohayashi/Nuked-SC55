// SC-55 のエフェクト（ERAM のリバーブ／コーラス）を浮動小数で書き直したもの。
//
// pcm.cpp の該当部は回路の転写で、20 ビットの値を 16 ビットの遅延メモリに詰めるための
// ブロック浮動小数（14 ビット仮数 + 2 ビット指数）と、シフトによる係数乗算でできている。
// 浮動小数にすると詰め替えは丸ごと不要になり、係数は実数の掛け算になる。
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
// 係数もタップもファームウェアが毎回書き換えるので、状態だけをこちらで持ち、
// 設定は pcm_t からそのつど読む。
#pragma once

#include <cstdint>
#include <cstring>

struct pcm_t;

struct PCMEffects
{
    static constexpr int DELAY_WORDS = 0x4000;   // 16,384 語 = 32 kHz で 512 ms

    float delay[DELAY_WORDS] {};                 // 遅延メモリ。整数版の eram に対応
    float a[8] {};                               // ram1[28][0..7] に対応する作業値
    float b[8] {};                               // ram1[29][0..7] に対応する作業値

    void reset() { std::memset(delay, 0, sizeof delay); std::memset(a, 0, sizeof a); std::memset(b, 0, sizeof b); }
};

// 20 ビットの範囲。整数版は sx20 で折り返すが、通常の動作では範囲に収まるので
// 浮動小数版では飽和させる（折り返しは音にならない）。
inline float PCMEffects_Clip (float v)
{
    constexpr float limit = 524288.0f;           // 2^19
    return v > limit - 1.0f ? limit - 1.0f : (v < -limit ? -limit : v);
}

void PCMEffects_Step (PCMEffects& fx, const pcm_t& pcm, uint16_t tv_counter,
                      float reverb_in, float chorus_in,
                      float rcadd[6], float rcadd2[6]);

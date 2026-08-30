// sc55_tables - ファームウェアが使っている ROM 上の変換表。
//
// どれも SC-55 v1.21 の ROM1 から取り出し、使っているコードを読んで用途を確かめたもの。
// ネイティブなボイスエンジンは、これらをそのまま引けば実機と同じ値になる。
// 出所は FIRMWARE_STRUCTURE.md に記録してある。
#pragma once

#include <cstdint>
#include <cmath>

namespace sc55
{

// パンの曲線。rom1[0x6c8f]、129 エントリ。
//
// ファームウェアは pan と 128-pan で 2 回引き、左を上位バイト、右を下位バイトに詰めて
// PCM の ram2[1] へ渡す（00:306c）。形は 127*sin(p/128 * pi/2) で、センターが 75/127
// = -4.6 dB。定パワーでも直線でもない。
inline constexpr uint8_t PAN_CURVE[129] = {
      0,   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  11,  12,  13,  14,  15,
     16,  17,  19,  20,  21,  22,  23,  24,  26,  27,  28,  29,  31,  32,  33,  34,
     35,  37,  38,  39,  40,  42,  43,  44,  46,  47,  48,  49,  51,  52,  53,  54,
     56,  57,  58,  59,  61,  62,  63,  64,  66,  67,  68,  69,  71,  72,  73,  74,
     75,  77,  78,  79,  80,  81,  83,  84,  85,  86,  87,  88,  89,  90,  91,  93,
     94,  95,  96,  97,  98,  99, 100, 101, 102, 103, 103, 104, 105, 106, 107, 108,
    109, 110, 110, 111, 112, 113, 114, 114, 115, 116, 116, 117, 118, 118, 119, 120,
    120, 121, 121, 122, 122, 123, 123, 124, 124, 125, 125, 126, 126, 126, 127, 127,
    127,
};

// エンベロープ速度の指数部。rom1[0x67ba]。
//
// 目標との差を左シフトで正規化して得た指数を、上位ニブルに置くための表（00:369c）。
// 11 以上は 0xff = 停止。下位ニブルには差の上位ビットから作った仮数が入る。
inline constexpr uint8_t ENVELOPE_EXPONENT[12] = {
      0,  16,  32,  48,  64,  80,  96, 112, 128, 144, 160, 255,
};

// 0 から 0x80 までの線形表。rom1[0x679a] に 21 エントリ、続けて 11 エントリ。
// mulxu.b で掛けてパラメータから時間や深さを補間するのに使う（00:2e9a ほか）。
inline constexpr uint8_t LINEAR_21[21] = {
      0,   6,  13,  19,  26,  32,  38,  45,  51,  58,  64,  70,  77,  83,  90,  96, 102, 109, 115, 122, 128,
};

inline constexpr uint8_t LINEAR_11[11] = {
      0,  13,  26,  38,  51,  64,  77,  90, 102, 115, 128,
};

// ピッチ。ファームウェアはピッチを 0.1 セント単位で持ち、2 の冪乗を 2 段の表引きで作る
// （00:5209-00:5234）。上位バイトで粗い表 rom1[0x7d7a] を、下位バイトで細かい表
// rom1[0x7b7a] を引き、掛けてからオクターブ数だけ右シフトする。
//
// 表は 303 エントリ全部が下の式の round() と完全に一致するので、ROM から取り出す
// 必要はない。データではなく式だった。
//
//   COARSE[h] = round(32768 * 2^(h * 25.6 / 1200))     h = 0..46   （1 段 = 25.6 セント）
//   FINE[l]   = round((2^(l * 0.1 / 1200) - 1) * 2^22)  l = 0..255  （1 段 = 0.1 セント）
//
// 粗い表が 47 段しかないのは、47 * 25.6 = 1203 セント、つまり 1 オクターブで
// 上位が 16 ビットを使い切るから。オクターブをまたぐぶんは右シフトが受け持つ。
inline uint16_t PitchCoarse (int high_byte)   // 0..46
{
    return (uint16_t) std::lround (32768.0 * std::exp2 (high_byte * 25.6 / 1200.0));
}

inline uint32_t PitchFine (int low_byte)      // 0..255
{
    return (uint32_t) std::lround ((std::exp2 (low_byte * 0.1 / 1200.0) - 1.0) * 4194304.0);
}

// deci_cents は 0.1 セント単位。12000 でちょうど 1 オクターブ。
inline float PitchRatio (float deci_cents)
{
    return std::exp2 (deci_cents / 12000.0f);
}

// ピッチワードの組み立て（00:51d5-00:5278）。
//
// ファームウェアはボイスごとに 24 ビットのアキュムレータを持ち、毎ティック増分を
// 足し込む（ポルタメント／ピッチエンベロープ）。そこから 24 ビットの基準値と
// 12000（= 1 オクターブ）を引いた差が、鳴らすべきピッチ。
//
//   acc = (voice+0x2d << 16) | voice+0x46 のワード
//   ref = (voice+0x29 << 16) | voice+0x3e のワード
//
// 差を 12000 で割ると商がオクターブ、余りが表引きの入力になる。負の側（下向き）は
// 絶対値を取ってから割り、余りがあればオクターブを 1 増やして余りを補数にする。
// 正の側は 1 オクターブを超えたら 0xffff で飽和する。
//
// 実機との照合: TOKMEDLY 14152/14152、IMAGA_55 17367/17367、GATCHA55 15011/15011。
inline uint16_t PitchLookup (uint32_t remainder)      // 余り 0..11999
{
    const uint32_t coarse = PitchCoarse ((int) ((remainder >> 8) & 0xff));
    const uint32_t fine   = PitchFine   ((int) (remainder & 0xff));
    return (uint16_t) (coarse + ((fine * coarse) >> 22));
}

inline uint16_t PitchWord (int32_t accumulator24, int32_t reference24)
{
    int32_t d = accumulator24 - reference24 - 12000;
    d = (int32_t) (d << 8) >> 8;                      // 24 ビット符号拡張

    if (d < 0)
    {
        const uint32_t magnitude = (uint32_t) -d;
        uint32_t octave = magnitude / 12000, remainder = magnitude % 12000;
        if (remainder != 0) { ++octave; remainder = 12000 - remainder; }
        return octave >= 16 ? 0 : (uint16_t) (PitchLookup (remainder) >> octave);
    }

    return (uint32_t) d / 12000 ? 0xffff : PitchLookup ((uint32_t) d % 12000);
}

} // namespace sc55


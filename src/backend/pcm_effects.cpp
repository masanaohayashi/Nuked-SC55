// pcm.cpp のエフェクト段（回路の転写）を浮動小数へ置き換えたもの。
//
// 対応は段番号で追える。整数版の
//
//   multi(x, (int8_t)(v >> 8)) >> 5      →  x * hi(v)      hi(v) = (int8_t)(v>>8) / 32
//   addclip20(a, m >> 1, m & 1)          →  a + m * 0.5f
//   addclip20(a, v ^ 0xfffff, 1)         →  a - v          （2 の補数の引き算）
//   eram_unpack(addr, 1)                 →  読み出しの半分
//
// ブロック浮動小数の詰め替え（14 ビット仮数 + 2 ビット指数）は、遅延メモリを float に
// した時点で不要になるのでどこにも現れない。
#include "pcm_effects.h"
#include "pcm.h"

namespace
{

inline float coef_hi (uint16_t v) { return (float) (int8_t) (v >> 8) * (1.0f / 32.0f); }
inline float coef_hi6(uint16_t v) { return (float) (int8_t) (v >> 8) * (1.0f / 64.0f); }
inline float coef_lo (uint16_t v) { return (float) (int8_t) (v & 255) * (1.0f / 32.0f); }

} // namespace

void PCMEffects_Step (PCMEffects& fx, const pcm_t& pcm, uint16_t tv,
                      float reverb_in, float chorus_in,
                      float rcadd[6], float rcadd2[6])
{
    const uint16_t* c28 = pcm.ram2[28];
    const uint16_t* c29 = pcm.ram2[29];
    const uint16_t* c30 = pcm.ram2[30];
    const uint16_t* c31 = pcm.ram2[31];

    float* a = fx.a;
    float* b = fx.b;

    const auto rd  = [&] (uint16_t tap) { return fx.delay[(uint32_t) (tap + tv) & 0x3fff]; };
    const auto rd1 = [&] (uint16_t tap) { return rd(tap) * 0.5f; };            // type=1
    const auto rdo = [&] (uint16_t tap) { return fx.delay[(uint32_t) (tap + tv + 1) & 0x3fff]; };
    const auto wr  = [&] (uint16_t tap, float v) { fx.delay[(uint32_t) (tap + tv) & 0x3fff] = PCMEffects_Clip(v); };

    // 入力段。リバーブとコーラスのバスを一次ローパスに通してから拡散段へ渡す。
    b[1] = (b[1] * coef_hi(c31[1]) + chorus_in * coef_lo(c31[1])) * 0.5f;
    b[0] = (b[0] * coef_hi(c30[1]) + reverb_in * coef_lo(c30[1])) * 0.5f;

    // 段 1-4: シュレーダー型オールパスの縦続。(v1 & 0x30) が段の有効・無効。
    {   // 1
        const uint16_t v = c30[4];
        const float s1 = rd1(c28[1]), s2 = rd(c28[1]);
        const float back = (v & 0x30) ? s1 : 0.0f;
        const float t = b[0] * coef_hi6(v) - back;
        b[4] = t;
        b[5] = t * coef_lo(v) * 0.5f + s2;
    }
    {   // 2
        const uint16_t v = c30[4];
        const float s1 = rd1(c28[2]), s2 = rd(c28[2]);
        const float back = (v & 0x30) ? s1 : 0.0f;
        const float t = b[5] - back;
        b[5] = t;
        a[0] = t * coef_lo(v) * 0.5f + s2;
    }
    {   // 3
        const uint16_t v = c30[4];
        const float s1 = rd1(c28[3]), s2 = rd(c28[3]);
        const float back = (v & 0x30) ? s1 : 0.0f;
        const float t = a[0] - back;
        a[0] = t;
        a[1] = t * coef_lo(v) * 0.5f + s2;
        a[2] = rd(c28[5]);
    }
    {   // 4
        const uint16_t v = c30[5];
        const float s1 = rd1(c28[4]), s2 = rd(c28[4]);
        const float back = (v & 0x30) ? s1 : 0.0f;
        const float t = a[1] - back;
        a[1] = t;
        a[3] = t * coef_lo(v) * 0.5f + s2;
        a[4] = rd(c29[1]);
    }

    // 段 5-6: 尾の減衰（一次ローパス）。ここで拡散段の出力を遅延メモリへ書き戻す。
    {   // 5
        const uint16_t v = c30[7];
        b[2] = (b[2] * coef_hi(v) + rd(c29[0]) * coef_lo(v)) * 0.5f;
        wr(c28[0], b[4]);
    }
    {   // 6
        const uint16_t v = c30[8];
        b[3] = (b[3] * coef_hi(v) + rd(c29[8]) * coef_lo(v)) * 0.5f;
        wr(c28[1], b[5]);
        wr(c28[2], a[0]);
    }
    {   // 7  左右に振り分ける
        const uint16_t v = c30[9];
        const float in = a[3];
        a[3] = in + b[2] * coef_hi(v) * 0.5f;
        a[5] = in + b[3] * coef_hi(v) * 0.5f;
        wr(c28[3], a[1]);
    }

    // 段 8-16: 尾のコムと出力の足し込み。
    {   // 8
        const uint16_t v = c30[6];
        const float t = a[3] + a[2] * coef_hi(v) * 0.5f;
        a[3] = t;
        a[2] = a[2] + t * coef_lo(v) * 0.5f;
        a[1] = rd(c28[9]);
    }
    {   // 9
        const uint16_t v = c30[6];
        const float t = a[5] + a[4] * coef_hi(v) * 0.5f;
        a[5] = t;
        a[4] = a[4] + t * coef_lo(v) * 0.5f;
        b[4] = rd(c29[5]);
    }
    {   // 10
        const uint16_t v = c30[6];
        const float in = a[1];
        const float t = in * coef_hi(v) * 0.5f + rd(c28[8]);
        a[1] = t;
        b[5] = t * coef_lo(v) * 0.5f + in;
        wr(c28[4], a[3]);
    }
    {   // 11
        const uint16_t v = c30[6];
        const float in = b[4];
        const float t = in * coef_hi(v) * 0.5f + rd(c29[4]);
        b[4] = t;
        a[0] = t * coef_lo(v) * 0.5f + in;
        wr(c28[5], a[2]);
        wr(c29[0], a[5]);
    }
    a[5] = rd(c28[6]);                                     // 12
    {   // 13
        a[5] = a[5] + rd(c28[10]);
        a[2] = rd(c29[2]);
    }
    {   // 14
        a[5] = (rd(c29[6]) + a[2]) + a[5];
        a[2] = rd(c28[7]);
    }
    {   // 15
        a[2] = a[2] + rd(c28[11]);
        a[3] = rd(c29[3]);
    }
    {   // 16
        a[2] = (rd(c29[7]) + a[2]) + a[3];
        wr(c29[1], a[4]);
        wr(c28[8], a[1]);
    }

    // 段 17-18: リバーブから出力バスへ。ここで次の周回ぶんのコーラス入力も読む。
    {   // 17
        const uint16_t v = c30[2];
        rcadd[0]  = a[5] * coef_hi(v);
        rcadd2[0] = a[5] * coef_lo(v);
        const float t = rdo(c29[10]);
        wr(c28[9], b[5]);
        b[5] = t;
    }
    {   // 18
        const uint16_t v = c30[3];
        rcadd[1]  = a[2] * coef_hi(v);
        rcadd2[1] = a[2] * coef_lo(v);
        a[1] = rdo(c29[11]);
    }

    // 段 19-20: コーラス。タップは LFO で動かされ、係数もその位相から作られる。
    {   // 19
        const uint16_t v = c31[9];
        const float s1 = rd(c29[10]);
        wr(c29[4], b[4]);
        b[5] = (s1 - s1 * coef_hi(v) * 0.5f) + b[5] * coef_hi(v) * 0.5f;
    }
    {   // 20
        const uint16_t v = c31[10];
        const float s1 = rd(c29[11]);
        wr(c29[5], a[0]);
        a[1] = (s1 - s1 * coef_hi(v) * 0.5f) + a[1] * coef_hi(v) * 0.5f;
        wr(c29[9], b[1]);
    }

    // 段 21-23, 31: コーラスから出力バスへ。
    rcadd[2]  = b[5] * coef_hi(c31[2]);   rcadd2[2] = b[5] * coef_lo(c31[2]);
    rcadd[3]  = b[5] * coef_hi(c31[3]);   rcadd2[3] = b[5] * coef_lo(c31[3]);
    rcadd[4]  = a[1] * coef_hi(c31[4]);   rcadd2[4] = a[1] * coef_lo(c31[4]);
    rcadd[5]  = a[1] * coef_hi(c31[5]);   rcadd2[5] = a[1] * coef_lo(c31[5]);

    for (int i = 0; i < 6; ++i) { rcadd[i] = PCMEffects_Clip(rcadd[i]); rcadd2[i] = PCMEffects_Clip(rcadd2[i]); }
    for (int i = 0; i < 8; ++i) { a[i] = PCMEffects_Clip(a[i]); b[i] = PCMEffects_Clip(b[i]); }
}

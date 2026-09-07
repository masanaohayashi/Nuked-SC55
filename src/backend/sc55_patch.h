// sc55_patch - SC-55 の音色データを ROM から直接読む。
//
// ファームウェアはこのテーブルを 216 バイトのブロックとして読み、そこから
// ボイスのパラメータを組み立てている。ブロックの形と、いくつかのフィールドの
// 意味は実測で確定した（FIRMWARE_STRUCTURE.md を参照）。ここはその読み口で、
// ネイティブなボイスエンジンが H8 を介さずに音色データへ到達するための土台。
//
// 分かっていないバイトは raw のまま置いてある。判明したものから名前がつく。
#pragma once

#include <cstdint>
#include <span>
#include <string>

// 1 パーシャルぶんのパラメータ。ブロック内に 2 つ並ぶ。
struct SC55Partial
{
    static constexpr int SIZE = 0x5c;   // 92 バイト

    // 旧ダンプ用の推測フィールド。3組とも同形式という解釈は未検証。
    // v1.21で確認済みの +0x4e..0x52 の展開は
    // sc55_envelope_setup.h を使う（この旧区分とは一致しない）。
    uint8_t envelope[3][5] {};     // +0x12-0x16, +0x4a-0x4e, +0x4f-0x53
    uint8_t envelope_flags[3] {};

    // まだ意味の分かっていないバイトを含む、ブロック内のそのままの並び。
    uint8_t raw[SIZE] {};

    // Multisample group (+2, big endian) is not the 0xffff absent sentinel.
    // Presence only: this does not include note/velocity-dependent gates.
    bool used = false;
};

// 1 音色。216 バイト。
struct SC55Patch
{
    static constexpr int SIZE = 0xd8;        // 216 バイト
    static constexpr int NAME_OFFSET = 0;
    static constexpr int NAME_LENGTH = 12;
    static constexpr int COMMON_OFFSET = 0x0c;
    static constexpr int COMMON_LENGTH = 0x14;
    static constexpr int PARTIAL_OFFSET = 0x20;

    std::string name;
    SC55Partial partial[2];
    uint8_t common[COMMON_LENGTH] {};
    uint32_t rom_offset = 0;
};

// ROM2 全体を渡すと、音色テーブルを見つけて読む。
//
// v1.21 はハッシュで特定して境界を適用。それ以外は音色名の間隔による推測で、
// データ境界の保証はない。判明している形式や抽出済みデータには loadRecords を使う。
class SC55PatchTable
{
public:
    bool load(std::span<const uint8_t> controlRom2);
    // Explicit data-only import: exactly these records, never scan adjacent
    // sample tables. Also accepts an extracted patch asset with start == 0.
    bool loadRecords(std::span<const uint8_t> data, uint32_t start, int recordCount);

    int size() const noexcept { return count; }
    const SC55Patch& operator[](int index) const { return patches[index]; }
    uint32_t base() const noexcept { return table_base; }

private:
    static constexpr int MAX_PATCHES = 256;

    SC55Patch patches[MAX_PATCHES];
    int count = 0;
    uint32_t table_base = 0;
};

// 音色パラメータの展開（ファームウェア 00:2ed7-）。
//
// パーシャルの 1 バイトが 2 つに割れてボイス構造体へ入る。bit7 はフラグとして
// voice-8+k へ 0 か 4 という形で、下位 7 ビットは値として voice+0x4f+k へ。
// v1.21で確認済みなのは partial+0x4e..0x52 → voice+0x4f..0x53。
// voice+0x60..0x69 を同じ形式とみなすことはできない。
//
// bit7 が立っていればフラグは 0、寝ていれば 4。逆に見えるが実機がそう書く。
//
// 実機との照合（値・フラグとも）:
//   14DIZZY 1,608/1,608   01HELP 1,551/1,551   02BEFORE 1,170/1,170
struct SC55ExpandedParameter
{
    uint8_t value = 0;   // voice+0x4f+k
    uint8_t flag  = 0;   // voice-8+k、0 か 4
};

inline SC55ExpandedParameter SC55_ExpandParameter (uint8_t source)
{
    return { (uint8_t) (source & 0x7f), (uint8_t) ((source & 0x80) ? 0 : 4) };
}

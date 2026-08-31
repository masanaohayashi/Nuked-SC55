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

    // 実測で用途の分かっているもの。
    // 5 セグメントのエンベロープが 3 組。各バイトは下位 7 ビットが値、bit7 がフラグ。
    // 展開しているのは 00:2eeb 前後、消費するのは PCM の 3 本の発生器。
    uint8_t envelope[3][5] {};     // +0x12-0x16, +0x4a-0x4e, +0x4f-0x53
    uint8_t envelope_flags[3] {};

    // まだ意味の分かっていないバイトを含む、ブロック内のそのままの並び。
    uint8_t raw[SIZE] {};

    // このパーシャルが使われているか。未使用なら先頭が 00/ff で埋まる。
    // 225 音色のうち丸ごと 0xff なのは番号 0 (Piano 1) だけで、これは ROM の
    // 実際の中身。読み取りの失敗ではない。
    bool used = false;
};

// 1 音色。216 バイト。
struct SC55Patch
{
    static constexpr int SIZE = 0xd8;        // 216 バイト
    static constexpr int NAME_OFFSET = 0xb8;
    static constexpr int NAME_LENGTH = 12;
    static constexpr int COMMON_OFFSET = 0xc4;
    static constexpr int COMMON_LENGTH = 0x14;

    std::string name;
    SC55Partial partial[2];
    uint8_t common[COMMON_LENGTH] {};
    uint32_t rom_offset = 0;
};

// ROM2 全体を渡すと、音色テーブルを見つけて読む。
//
// テーブルの位置は決め打ちしない。音色名が 216 バイト間隔で並ぶ場所を探すので、
// ROM のバージョンが変わっても追従する。見つからなければ空を返す。
class SC55PatchTable
{
public:
    bool load(std::span<const uint8_t> waverom2);

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
// 5 要素ずつの組が 3 つある（+0x4f-0x53、+0x60-0x64、+0x65-0x69）。
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

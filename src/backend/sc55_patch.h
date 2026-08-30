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

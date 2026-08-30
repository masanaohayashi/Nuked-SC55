#!/usr/bin/env python3
"""SC-55 の音色テーブルを ROM2 から読む。

    python3 tools/sc55patches.py "/path/to/SC-55 v1.21/sc55_rom2.bin"

音色名が 0xd8 間隔で並んでいることを手がかりにテーブルの先頭を自力で探すので、
ROM のバージョンが違ってもアドレスを決め打ちしなくてよい。

  パッチブロック 216 バイト
    +0x00-0x5b   パーシャル 1 (92 バイト)
    +0x5c-0xb7   パーシャル 2 (92 バイト)
    +0xb8-0xc3   音色名 (ASCII 12)
    +0xc4-0xd7   共通部 (20 バイト)
"""
import sys

STRIDE, NAME_OFF, NAME_LEN, PARTIAL = 0xd8, 0xb8, 12, 0x5c


def find_table(rom):
    """名前が連続して読める場所のうち、いちばん長く続くものを取る。"""
    best = (0, 0)
    for start in range(0, len(rom) - STRIDE * 8, 4):
        n = 0
        while start + n * STRIDE + NAME_OFF + NAME_LEN <= len(rom):
            name = rom[start + n * STRIDE + NAME_OFF:][:NAME_LEN]
            if not all(32 <= c < 127 for c in name):
                break
            n += 1
        if n > best[1]:
            best = (start, n)
    return best


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1

    rom = open(sys.argv[1], 'rb').read()
    start, count = find_table(rom)
    if count < 8:
        print('音色テーブルが見つからない')
        return 1

    print('rom2[%05x] から %d 音色, %d バイト間隔' % (start, count, STRIDE))
    for i in range(count):
        base = start + i * STRIDE
        name = rom[base + NAME_OFF:][:NAME_LEN].decode('ascii').rstrip()
        # パーシャル 2 が空かどうかは先頭バイト群で見分けがつく
        second = rom[base + PARTIAL:][:16]
        used = 2 if any(b not in (0x00, 0xff) for b in second[:8]) else 1
        print('  %3d  %05x  %-12s  パーシャル %d' % (i, base, name, used))
    return 0


if __name__ == '__main__':
    sys.exit(main())

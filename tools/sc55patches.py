#!/usr/bin/env python3
"""SC-55 の音色テーブルを ROM2 から読む。

    python3 tools/sc55patches.py "/path/to/SC-55 v1.21/sc55_rom2.bin"
    python3 tools/sc55patches.py ROM --bank 2 --export /tmp/sc55-v121-bank2.patches

v1.21 は SHA-256 で特定し、波形テーブルの直前で終了する。
他の版は音色名が 0xd8 間隔で並ぶ場所を推測する（境界は未保証）。
--bank 2 はハッシュ一致する v1.21 限定。追加音色・ドラムの162レコード。
--export は指定バンクのレコードのみを新規ファイルに保存（上書き不可）。
波形グループ、ドラムセット設定、命令コードはこの出力に含まれない。

  パッチブロック 216 バイト
    +0x00-0x0b   音色名 (ASCII 12)
    +0x0c-0x1f   共通部 (20 バイト)
    +0x20-0x7b   パーシャル 1 (92 バイト)
    +0x7c-0xd7   パーシャル 2 (92 バイト)
"""
import sys
import hashlib
import argparse

STRIDE, NAME_OFF, NAME_LEN, PARTIAL = 0xd8, 0, 12, 0x5c


def find_table(rom, bank=1):
    """名前が連続して読める場所のうち、いちばん長く続くものを取る。"""
    if hashlib.sha256(rom).hexdigest() == 'effc6132d68f7e300aaef915ccdd08aba93606c22d23e580daf9ea6617913af1':
        return (0x20000, 162) if bank == 2 else (0x10000, 224)
    if bank == 2:
        raise ValueError('bank 2 の境界は SC-55 v1.21 の既知ハッシュでのみ確認済みです')
    best = (0, 0)
    for start in range(0, len(rom) - STRIDE * 8 + 1, 4):
        n = 0
        while start + (n + 1) * STRIDE <= len(rom):
            name = rom[start + n * STRIDE + NAME_OFF:][:NAME_LEN]
            if not all(32 <= c < 127 for c in name):
                break
            n += 1
        if n > best[1]:
            best = (start, n)
    return best


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('rom')
    parser.add_argument('--bank', type=int, choices=(1, 2), default=1)
    parser.add_argument('--export', metavar='NEW_FILE')
    args = parser.parse_args()
    with open(args.rom, 'rb') as source:
        rom = source.read()
    try:
        start, count = find_table(rom, args.bank)
    except ValueError as error:
        parser.error(str(error))
    if count < 8:
        print('音色テーブルが見つからない')
        return 1

    if args.export:
        # An inferred printable-name run is not a safe extraction boundary.
        if hashlib.sha256(rom).hexdigest() != 'effc6132d68f7e300aaef915ccdd08aba93606c22d23e580daf9ea6617913af1':
            parser.error('export requires the verified v1.21 ROM hash')
        payload = rom[start:start + count * STRIDE]
        try:
            with open(args.export, 'xb') as output:
                if output.write(payload) != len(payload):
                    raise OSError('incomplete patch export')
        except OSError as error:
            parser.error(str(error))
        print('Exported bank %d: %d records / %d data-only bytes -> %s'
              % (args.bank, count, len(payload), args.export))
        return 0

    print('rom2[%05x] から %d 音色, %d バイト間隔' % (start, count, STRIDE))
    for i in range(count):
        base = start + i * STRIDE
        name = rom[base + NAME_OFF:][:NAME_LEN].decode('ascii').rstrip()
        # v1.21: group 0xffff denotes an absent partial, independently of
        # other bytes and note/velocity-dependent selection gates.
        used = sum(rom[base + 0x20 + p * PARTIAL + 2:
                       base + 0x20 + p * PARTIAL + 4] != b'\xff\xff'
                   for p in range(2))
        print('  %3d  %05x  %-12s  パーシャル %d' % (i, base, name, used))
    return 0


if __name__ == '__main__':
    sys.exit(main())

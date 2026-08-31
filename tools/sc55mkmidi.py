#!/usr/bin/env python3
# sc55mkmidi.py - エフェクトを調べるための試験用 .mid を作る。
#
# 曲を借りてくると、リバーブの種類もパラメータも曲任せになって条件が固定できない。
# GS の SysEx で明示的に設定した短いファイルを作り、1 音だけ鳴らして尾を測る。
#
#   python3 tools/sc55mkmidi.py <出力ディレクトリ>

import os, struct, sys

def vlq(n):
    out = bytearray([n & 0x7f]); n >>= 7
    while n: out.insert(0, (n & 0x7f) | 0x80); n >>= 7
    return bytes(out)

def gs(addr, data):
    """Roland GS SysEx。アドレス 3 バイト + データ、末尾にチェックサム。"""
    body = bytes(addr) + bytes(data)
    checksum = (128 - (sum(body) % 128)) % 128
    return b'\xf0\x41\x10\x42\x12' + body + bytes([checksum]) + b'\xf7'

def track(events):
    """events は (デルタtick, バイト列) の並び。"""
    out = bytearray()
    for delta, msg in events:
        out += vlq(delta)
        if msg[0] == 0xf0:
            out += b'\xf0' + vlq(len(msg) - 1) + msg[1:]
        else:
            out += msg
    out += b'\x00\xff\x2f\x00'
    return b'MTrk' + struct.pack('>I', len(out)) + bytes(out)

def write(path, events, division=480):
    head = b'MThd' + struct.pack('>IHHH', 6, 0, 1, division)
    open(path, 'wb').write(head + track(events))

GS_RESET = gs((0x40, 0x00, 0x7f), (0x00,))

def test_file(path, extra, note=60, program=48, send=127, hold=480, tail=480 * 8):
    ev = [(0, GS_RESET), (240, b'')]
    ev = [(0, GS_RESET)]
    t = 240
    for msg in extra:
        ev.append((t, msg)); t = 0
    ev.append((t if t else 0, bytes([0xc0, program])))
    ev.append((0, bytes([0xb0, 91, send])))          # リバーブ送り
    ev.append((0, bytes([0xb0, 93, 0])))             # コーラス送りは切る
    ev.append((240, bytes([0x90, note, 100])))
    ev.append((hold, bytes([0x80, note, 0])))
    ev.append((tail, bytes([0xb0, 120, 0])))         # 末尾まで無音を伸ばす
    write(path, ev)

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(out, exist_ok=True)
    made = []

    # リバーブの種類（マクロ 0..7）
    names = ['room1', 'room2', 'room3', 'hall1', 'hall2', 'plate', 'delay', 'pandelay']
    for i, name in enumerate(names):
        p = os.path.join(out, f'rev_{i}_{name}.mid')
        test_file(p, [gs((0x40, 0x01, 0x30), (i,))]); made.append(p)

    # リバーブタイム（Hall2 固定で 0/32/64/96/127）
    for tm in (0, 32, 64, 96, 127):
        p = os.path.join(out, f'revtime_{tm:03d}.mid')
        test_file(p, [gs((0x40, 0x01, 0x30), (4,)), gs((0x40, 0x01, 0x34), (tm,))]); made.append(p)

    # リバーブレベル
    for lv in (0, 64, 127):
        p = os.path.join(out, f'revlevel_{lv:03d}.mid')
        test_file(p, [gs((0x40, 0x01, 0x30), (4,)), gs((0x40, 0x01, 0x33), (lv,))]); made.append(p)

    # コーラス（送りを入れ替える）
    for i in range(8):
        p = os.path.join(out, f'cho_{i}.mid')
        ev = [(0, GS_RESET), (240, gs((0x40, 0x01, 0x38), (i,))),
              (0, bytes([0xc0, 48])), (0, bytes([0xb0, 91, 0])), (0, bytes([0xb0, 93, 127])),
              (240, bytes([0x90, 60, 100])), (480, bytes([0x80, 60, 0])),
              (480 * 8, bytes([0xb0, 120, 0]))]
        write(p, ev); made.append(p)

    # 送りゼロ（比較用の底）
    p = os.path.join(out, 'dry.mid'); test_file(p, [], send=0); made.append(p)

    print(f'{len(made)} 個作成: {out}')

main()

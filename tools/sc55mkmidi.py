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

    # GS のエフェクトパラメータ掃引。40 01 30..3f を 1 つずつ 2 値で作り、
    # 実行後のメモリを差分すると、そのパラメータがどこに入るかが出る。
    sweep = os.path.join(out, 'gsparam'); os.makedirs(sweep, exist_ok=True)
    for lo in range(0x30, 0x40):
        for tag, val in (('a', 0x00), ('b', 0x05)):
            ev = [(0, GS_RESET), (240, gs((0x40, 0x01, lo), (val,))),
                  (240, bytes([0xb0, 120, 0]))]
            write(os.path.join(sweep, f'p{lo:02x}_{tag}.mid'), ev); made.append('sweep')

    # NRPN 掃引。CC71-74 が効かない代わりに SC-55 が使う経路。
    nr = os.path.join(out, 'nrpn'); os.makedirs(nr, exist_ok=True)
    NRPN = [(0x01, 0x08), (0x01, 0x09), (0x01, 0x0a), (0x01, 0x20), (0x01, 0x21),
            (0x01, 0x63), (0x01, 0x64), (0x01, 0x66),
            (0x18, 0x24), (0x1a, 0x24), (0x1c, 0x24), (0x1d, 0x24), (0x1e, 0x24)]
    for msb, lsb in NRPN:
        ch = 9 if msb >= 0x18 else 0            # ドラム系はチャンネル 10
        for tag, val in (('a', 0x40), ('b', 0x10)):
            ev = [(0, GS_RESET),
                  (240, bytes([0xb0 | ch, 99, msb])), (0, bytes([0xb0 | ch, 98, lsb])),
                  (0, bytes([0xb0 | ch, 6, val])), (240, bytes([0xb0 | ch, 120, 0]))]
            write(os.path.join(nr, f'n{msb:02x}{lsb:02x}_{tag}.mid'), ev); made.append('nrpn')

    # パート単位の GS パラメータ 40 11 xx。
    #
    # 値の選び方に注意。範囲外の値は弾かれるので、「検証して拒否する」パラメータと
    # 「そもそも実装されていない」パラメータが同じに見える。受信チャンネル（02）を
    # 0x40/0x10 で試して「保存されない」と誤って結論しかけた。0/5 なら保存される。
    pp = os.path.join(out, 'gspart'); os.makedirs(pp, exist_ok=True)
    for k in list(range(0x02, 0x24)) + list(range(0x30, 0x4c)):
        for tag, val in (('a', 0x00), ('b', 0x01)):
            ev = [(0, GS_RESET), (240, gs((0x40, 0x11, k), (val,))), (240, bytes([0xb0, 120, 0]))]
            write(os.path.join(pp, f'p{k:02x}_{tag}.mid'), ev); made.append('gspart')

    # 送りゼロ（比較用の底）
    p = os.path.join(out, 'dry.mid'); test_file(p, [], send=0); made.append(p)

    print(f'{len(made)} 個作成: {out}')

main()

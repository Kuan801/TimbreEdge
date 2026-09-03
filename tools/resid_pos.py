#!/usr/bin/env python3
# =============================================================================
#  resid_pos.py  -  殘差落在哪個「絕對頻率」上
#
#  用法
#  -----
#    python3 resid_pos.py --lib /path/to/聲音庫                  # 只看素材
#    python3 resid_pos.py --lib /path/to/聲音庫 --syn out.wav \
#            --inst piano --start 60 --count 24                  # 素材 vs 合成
#
#  為什麼需要這支：evaluate.py 的「噪聲位置」量的是殘差在四個固定頻帶的
#  *分佈*（百分比），那是相對量。可是「音頭沙沙聲」是絕對量的問題 ——
#  某個頻帶的噪聲比樂器自己還大聲。分佈對了不代表位階對了。
#
#  這支輸出兩種東西：
#    1) 殘差能量的 10/50/90 百分位頻率（Hz）—— 殘差「住在哪裡」
#    2) 各頻帶殘差相對整段能量的絕對位階（dB）—— 合成端可以直接跟素材比
#
#  實測結論（見 README「起音沙沙聲」一節）：百分位幾乎不隨音高變動，
#  小號從 165 Hz 到 1007 Hz（2.5 個八度），殘差中位數一直在 1.3~2.0 kHz。
#  殘差的落點是樂器本體的性質，不是音高的函數。
# =============================================================================

import argparse
import glob
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from evaluate import read_wav, split_scale, midi_of_filename, SR   # noqa: E402

BANDS = [(0, 900), (900, 2000), (2000, 5000), (5000, 22050)]


def residual(x, f0, t0, dur):
    """週期差分殘差。回傳 (殘差, 原訊號段)，取不到回 (None, None)。

    跟 evaluate.py 的 aperiodic() same 想法，但這裡要保留頻譜本身，
    不只是頻帶佔比。
    """
    k = int(0.01 * SR)
    e = np.array([np.sqrt(np.mean(x[i:i + k] ** 2))
                  for i in range(0, max(len(x) - k, 1), k)])
    if e.size == 0 or e.max() <= 0:
        return None, None
    on = int(np.argmax(e > 0.08 * e.max())) * k
    seg = x[on + int(t0 * SR): on + int((t0 + dur) * SR)]
    T = int(round(SR / f0))
    if len(seg) < T + 2048:
        return None, None
    n = (len(seg) - T) // 1024 * 1024
    if n < 1024:
        return None, None
    # 除以 sqrt(2)：不相關殘差經過差分之後能量會加倍（E[d^2] = 2E[e^2]）
    return (seg[T:T + n] - seg[:n]) / np.sqrt(2.0), seg[:n]


def percentiles(res):
    """殘差能量的 10/50/90 百分位頻率（Hz）。"""
    w = np.hanning(len(res))
    m = np.abs(np.fft.rfft(res * w)) ** 2
    fr = np.fft.rfftfreq(len(res), 1 / SR)
    c = np.cumsum(m)
    if c[-1] <= 0:
        return None
    c = c / c[-1]
    return [float(np.interp(q, c, fr)) for q in (0.1, 0.5, 0.9)]


def band_levels(res, sig):
    """各頻帶的殘差能量，相對「整段訊號總能量」的 dB。

    用整段總能量當分母（而不是同頻帶的訊號能量）是刻意的：
    要比的是「這一層在整個音裡有多大聲」，那才跟遮蔽與否有關。
    """
    w = np.hanning(len(res))
    D = np.abs(np.fft.rfft(res * w)) ** 2
    S = np.abs(np.fft.rfft(sig * w)) ** 2
    fr = np.fft.rfftfreq(len(res), 1 / SR)
    tot = S.sum() + 1e-20
    return [10 * np.log10((D[(fr >= lo) & (fr < hi)].sum() + 1e-20) / tot)
            for lo, hi in BANDS]


def collect_refs(inst_dir):
    out = {}
    for p in (sorted(glob.glob(os.path.join(inst_dir, "*.wav"))) +
              sorted(glob.glob(os.path.join(inst_dir, "*.WAV")))):
        m = midi_of_filename(p)
        if m is not None:
            out[m] = p
    return out


def report(name, rows_p, rows_b, label):
    if not rows_p:
        return
    p = np.median(np.array(rows_p), axis=0)
    b = np.median(np.array(rows_b), axis=0)
    print(f"{name:10} {label:6} "
          f"p10 {p[0]:6.0f}  p50 {p[1]:6.0f}  p90 {p[2]:6.0f}   |  "
          + "  ".join(f"{v:+6.1f}" for v in b))


def main():
    ap = argparse.ArgumentParser(
        description="殘差的絕對頻率落點與各頻帶位階")
    ap.add_argument("--lib", required=True, help="聲音庫根目錄")
    ap.add_argument("--inst", default=None, help="只跑某一種樂器")
    ap.add_argument("--syn", default=None,
                    help="要一起比對的合成半音階 WAV")
    ap.add_argument("--start", type=int, default=60,
                    help="合成檔第一個音的 MIDI 音高")
    ap.add_argument("--count", type=int, default=24, help="合成檔有幾個音")
    ap.add_argument("--window", default="attack",
                    choices=("attack", "sustain"),
                    help="attack = 起音 0~120 ms；sustain = 0.4~1.6 s")
    args = ap.parse_args()

    t0, dur = (0.0, 0.12) if args.window == "attack" else (0.4, 1.2)

    insts = ([args.inst] if args.inst else
             sorted(d for d in os.listdir(args.lib)
                    if os.path.isdir(os.path.join(args.lib, d))))

    print(f"視窗 {t0:.2f}~{t0 + dur:.2f} s   "
          f"位階單位 dB（相對整段能量），頻帶 "
          + " ".join(f"{lo}-{hi}" for lo, hi in BANDS))
    print("-" * 96)

    syn = None
    if args.syn:
        syn = dict(split_scale(read_wav(args.syn),
                               n_notes=args.count, start_midi=args.start))

    for inst in insts:
        refs = collect_refs(os.path.join(args.lib, inst))
        if len(refs) < 3:
            continue
        rp, rb, sp, sb = [], [], [], []
        for m, path in sorted(refs.items()):
            f0 = 440.0 * 2 ** ((m - 69) / 12.0)
            res, sig = residual(read_wav(path), f0, t0, dur)
            if res is not None:
                q = percentiles(res)
                if q:
                    rp.append(q)
                    rb.append(band_levels(res, sig))
            if syn is not None and m in syn:
                res, sig = residual(syn[m], f0, t0, dur)
                if res is not None:
                    q = percentiles(res)
                    if q:
                        sp.append(q)
                        sb.append(band_levels(res, sig))
        report(inst, rp, rb, "素材")
        report("", sp, sb, "合成")


if __name__ == "__main__":
    main()

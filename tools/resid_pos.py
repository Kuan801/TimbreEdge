#!/usr/bin/env python3
# =============================================================================
#  resid_pos.py  -  which "absolute frequency" the residual sits at
#
#  Usage
#  -----
#    python3 resid_pos.py --lib /path/to/聲音庫                  # samples only
#    python3 resid_pos.py --lib /path/to/聲音庫 --syn out.wav \
#            --inst piano --start 60 --count 24                  # samples vs synth
#
#  Why this exists: evaluate.py's "noise position" measures the *distribution* of the
#  residual over four fixed bands (as percentages), which is a relative quantity. But
#  the "attack hiss" is an absolute problem -- in some band the noise is simply louder
#  than the instrument itself. Getting the distribution right does not mean the level
#  is right.
#
#  This one prints two things:
#    1) the 10/50/90th percentile frequencies of the residual energy (Hz) -- where the
#       residual "lives"
#    2) the absolute level of the residual per band relative to the whole-segment
#       energy (dB) -- so the synth side can be compared directly against the samples
#
#  What the measurements showed (see the "attack hiss" section of the README): the
#  percentiles barely move with pitch. For the trumpet, from 165 Hz to 1007 Hz (2.5
#  octaves), the residual median stays at 1.3~2.0 kHz. Where the residual lands is a
#  property of the instrument itself, not a function of pitch.
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
    """Period-difference residual. Returns (residual, original signal segment), or (None, None) if it cannot be taken.

    Same idea as evaluate.py's aperiodic(), but here the spectrum itself has to be
    kept, not just the per-band shares.
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
    # Divide by sqrt(2): differencing doubles the energy of an uncorrelated residual (E[d^2] = 2E[e^2])
    return (seg[T:T + n] - seg[:n]) / np.sqrt(2.0), seg[:n]


def percentiles(res):
    """10/50/90th percentile frequencies of the residual energy (Hz)."""
    w = np.hanning(len(res))
    m = np.abs(np.fft.rfft(res * w)) ** 2
    fr = np.fft.rfftfreq(len(res), 1 / SR)
    c = np.cumsum(m)
    if c[-1] <= 0:
        return None
    c = c / c[-1]
    return [float(np.interp(q, c, fr)) for q in (0.1, 0.5, 0.9)]


def band_levels(res, sig):
    """Residual energy per band, in dB relative to the total energy of the whole segment.

    Using the whole-segment total as the denominator (rather than the signal energy in
    the same band) is deliberate: what we want to compare is "how loud this layer is
    within the whole note", and that is what decides whether it gets masked.
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

#!/usr/bin/env python3
# =============================================================================
#  evaluate.py  -  objective comparison: synth scale vs. original single notes
#
#  Usage
#  -----
#    python3 evaluate.py SCALE.WAV  /path/to/Piano.mf.*.wav
#    python3 evaluate.py SCALE.WAV  samples/ --plot report.png --json report.json
#
#  SCALE.WAV is the firmware's recording of the chromatic scale (press w to save
#  it as PLAY.WAV, or produce one with the simulator). Sample filenames have to
#  show the note name (C4 / Db4 / A4 ...); pairing is automatic.
#
#  Now that the range is no longer hard-coded C3~B4 but "bank range + one
#  octave", --start has to be given too: with C4~B4 material the synthesised
#  file is C4~B5, so --start 60 --count 24.
#
#  Five metrics, all "smaller is better" except the correlations
#  -------------------------------------------------------------
#   1. envelope correlation  Pearson r, compared in the log domain (ears are log)
#   2. decay time error      ratio of the times to fall -20 dB, given in cents
#   3. harmonic LSD          log-spectral distance (dB), -45 dB floor so zeros can't blow it up
#   4. spectrogram distance  mean absolute difference over mel band × time (dB)
#   5. centroid correlation  does brightness move the right way over time
#
#  Why the floor and the alignment:
#    computed raw, LSD is dragged to hundreds of dB by "high harmonics whose
#    true value is 0"; without aligning the onsets every time-based metric warps.
# =============================================================================

import argparse
import glob
import os
import re
import sys
import wave

import numpy as np

SR = 44100

# Below this log-domain std of the reference envelope, envelope correlation is not reported (see compare_note)
ENV_FLAT_STD_DB = 3.0
NOTE_NAMES = ["C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"]
ALT = {"C#": "Db", "D#": "Eb", "F#": "Gb", "G#": "Ab", "A#": "Bb"}


# =============================================================================
#  Interpretation criteria
#
#  The report generator (report_docx.py) imports this table directly -- do not
#  keep a second copy. Two copies means "the threshold was changed on one side
#  only" while the colours and conclusions in the report stay as they were --
#  that kind of error shows no symptom at all, it just quietly makes the report
#  wrong.
#
#    key       -> (display name, formatter, good, acceptable, one-line note)
#  "good" and "acceptable" are both lambdas; True means the value is in that band.
# =============================================================================
CRITERIA = {
    "env_r": ("包絡相關性 r", lambda v: f"{v:.3f}",
              lambda v: v > 0.95, lambda v: v > 0.85,
              "> 0.95 很好；0.85~0.95 可接受；< 0.85 包絡不對"),
    "decay_cents": ("衰減時間誤差", lambda v: f"{v:+.0f}c",
                    lambda v: abs(v) < 200, lambda v: abs(v) < 600,
                    "< 200 cent（約 1.12 倍）算對；> 600 cent 一聽就是不同樂器"),
    "lsd_atk": ("LSD 起音", lambda v: f"{v:.1f}",
                lambda v: v < 3, lambda v: v < 6,
                "諧波分佈的對數頻譜距離（dB）。< 3 很好；3~6 可接受；> 8 音色明顯不同"),
    "lsd_mid": ("LSD 中段", lambda v: f"{v:.1f}",
                lambda v: v < 3, lambda v: v < 6, "同上"),
    "lsd_late": ("LSD 尾段", lambda v: f"{v:.1f}",
                 lambda v: v < 3, lambda v: v < 6, "同上"),
    "mel_mae": ("頻譜圖 MAE", lambda v: f"{v:.1f}",
                lambda v: v < 3, lambda v: v < 6,
                "mel 頻帶×時間的平均絕對差（dB）。本底約 1.3 dB —— "
                "樂器本身的隨機成分無法逐點重現"),
    "cent_r": ("質心相關性 r", lambda v: f"{v:.3f}",
               lambda v: v > 0.9, lambda v: v > 0.8,
               "亮度隨時間的走向。> 0.9 正確"),
    "noise_db": ("噪聲量誤差", lambda v: f"{v:+.1f}dB",
                 lambda v: abs(v) < 3, lambda v: abs(v) < 10,
                 "非週期成分的份量。|x| < 3 dB 很好；> 10 dB 一聽就少了呼吸感"),
    "noise_pos": ("噪聲位置", lambda v: f"{v:.1f}pt",
                  lambda v: v < 8, lambda v: v < 15,
                  "非週期成分落在哪些頻帶（4 帶百分點 MAE）。< 8 pt 很好"),
}
METRIC_KEYS = list(CRITERIA)


def grade(key, v):
    """Return 'good' / 'ok' / 'bad' / 'na'."""
    if v is None or (isinstance(v, float) and np.isnan(v)):
        return "na"
    _, _, good, ok, _ = CRITERIA[key]
    return "good" if good(v) else ("ok" if ok(v) else "bad")


# ----------------------------------------------------------------- I/O -----
def read_wav(path):
    with wave.open(path, "rb") as w:
        sr, n, ch, sw = w.getframerate(), w.getnframes(), w.getnchannels(), w.getsampwidth()
        raw = w.readframes(n)
    if sw != 2:
        raise ValueError(f"{path}: 只支援 16-bit PCM")
    x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    if sr != SR:
        t = np.arange(len(x)) / sr
        x = np.interp(np.arange(0, t[-1], 1 / SR), t, x)
    return x


def midi_of_filename(path):
    """Pull the MIDI pitch out of a filename like Piano.mf.Db4.wav."""
    base = os.path.basename(path)
    m = re.search(r"\.([A-G][b#]?)(-?\d)\.", base) or re.search(r"([A-G][b#]?)(-?\d)", base)
    if not m:
        return None
    name, octv = m.group(1), int(m.group(2))
    name = ALT.get(name, name)
    if name not in NOTE_NAMES:
        return None
    return 12 * (octv + 1) + NOTE_NAMES.index(name)


def midi_name(m):
    return f"{NOTE_NAMES[m % 12]}{m // 12 - 1}"


# -------------------------------------------------- basic measurements -----
def envelope(x, hop=0.005):
    k = int(hop * SR)
    n = (len(x) - k) // k
    return np.array([np.sqrt(np.mean(x[i * k:i * k + k] ** 2)) for i in range(max(n, 1))])


def find_onset(e, thresh=0.08):
    m = e.max()
    if m <= 0:
        return 0
    idx = np.where(e > thresh * m)[0]
    return int(idx[0]) if len(idx) else 0


def decay_time_db(e, db=20.0):
    """Seconds to fall db decibels from the peak (hop 0.005s). Returns nan if it never does."""
    if e.max() <= 0:
        return np.nan
    pk = int(np.argmax(e))
    target = e[pk] * (10 ** (-db / 20))
    for i in range(pk, len(e)):
        if e[i] <= target:
            return (i - pk) * 0.005
    return np.nan


def refine_f0(x, f0_nom, t0, N=8192, tol=0.06):
    """Pin down f0 from the signal's own spectrum instead of trusting equal temperament.

    Why this is needed: real playing is off pitch. A measured trumpet B5
    recording has a fundamental of 1006.5 Hz, 32 cents above equal
    temperament's 987.8. harmonic_dist used to sample at h*f0_nom with a search
    window of only +-27 Hz, so by the 2nd harmonic it was already 37 Hz out --
    it measured the valleys between harmonics rather than the peaks, and the
    whole harmonic distribution came out wrong. That leaves the conclusion "high
    notes synthesise badly" with no ground to stand on (what was wrong was the
    ruler).

    Method: find the strongest peak within +-6% of f0_nom, then parabolic
    interpolation for sub-bin precision.
    """
    a = int(t0 * SR)
    s = x[a:a + N]
    if len(s) < N:
        s = np.pad(s, (0, N - len(s)))
    mag = np.abs(np.fft.rfft(s * np.hanning(N)))
    fr = np.fft.rfftfreq(N, 1 / SR)
    lo = int(f0_nom * (1 - tol) / (SR / N))
    hi = int(f0_nom * (1 + tol) / (SR / N))
    lo, hi = max(lo, 1), min(hi, len(mag) - 2)
    if hi <= lo:
        return f0_nom
    k = lo + int(np.argmax(mag[lo:hi + 1]))
    # parabolic interpolation, for sub-bin precision
    y0, y1, y2 = mag[k - 1], mag[k], mag[k + 1]
    den = 2 * (2 * y1 - y0 - y2)
    d = (y2 - y0) / den if abs(den) > 1e-12 else 0.0
    return float((k + d) * SR / N)


def harmonic_dist(x, f0, t0, n=24, N=8192):
    a = int(t0 * SR)
    s = x[a:a + N]
    if len(s) < N:
        s = np.pad(s, (0, N - len(s)))
    mag = np.abs(np.fft.rfft(s * np.hanning(N)))
    fr = np.fft.rfftfreq(N, 1 / SR)
    out = []
    for h in range(1, n + 1):
        f = f0 * h
        if f > SR * 0.45:
            out.append(0.0)
            continue
        k = int(np.argmin(np.abs(fr - f)))
        out.append(mag[max(k - 5, 0):k + 6].max())
    a = np.array(out)
    return a / max(a.sum(), 1e-12)


def lsd(a, b, floor_db=-45.0):
    """Log-spectral distance. Floor both sides, or harmonics whose true value is 0 drag the metric to hundreds of dB."""
    A = np.maximum(20 * np.log10(a + 1e-12), floor_db)
    B = np.maximum(20 * np.log10(b + 1e-12), floor_db)
    return float(np.sqrt(np.mean((A - B) ** 2)))


MEL_FLOOR_DB = -30.0


def mel_spectrogram(x, n_mels=48, N=2048, hop=512, fmin=50, fmax=16000, dur=2.0):
    x = x[:int(dur * SR)]
    if len(x) < N:
        x = np.pad(x, (0, N - len(x)))
    nfr = (len(x) - N) // hop + 1
    w = np.hanning(N)
    S = np.array([np.abs(np.fft.rfft(x[i * hop:i * hop + N] * w)) for i in range(nfr)])
    fr = np.fft.rfftfreq(N, 1 / SR)
    # triangular filterbank on a log frequency axis
    edges = np.exp(np.linspace(np.log(fmin), np.log(fmax), n_mels + 2))
    M = np.zeros((n_mels, len(fr)))
    for i in range(n_mels):
        lo, ce, hi = edges[i], edges[i + 1], edges[i + 2]
        left = (fr >= lo) & (fr < ce)
        right = (fr >= ce) & (fr < hi)
        M[i, left] = (fr[left] - lo) / max(ce - lo, 1e-9)
        M[i, right] = (hi - fr[right]) / max(hi - ce, 1e-9)
    return 20 * np.log10(S @ M.T + 1e-9)


def centroid_track(x, N=4096, hop=1024, dur=2.0):
    """Return (centroid track, per-frame energy). The energy comes back too because
    the centroid has to be energy-weighted -- in a quiet stretch any noise floor
    pushes the centroid way up. Measured on a violin crescendo sample: equal
    weighting gives 1433 Hz, energy weighting 414 Hz, a factor of 3.5."""
    x = x[:int(dur * SR)]
    if len(x) < N:
        x = np.pad(x, (0, N - len(x)))
    fr = np.fft.rfftfreq(N, 1 / SR)
    cen, eng = [], []
    for i in range((len(x) - N) // hop + 1):
        m = np.abs(np.fft.rfft(x[i * hop:i * hop + N] * np.hanning(N))) ** 2
        e = np.sum(m)
        cen.append(np.sum(fr * m) / max(e, 1e-12))
        eng.append(e)
    return np.array(cen), np.array(eng)


def pearson(a, b):
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    if n < 4 or np.std(a) < 1e-12 or np.std(b) < 1e-12:
        return np.nan
    return float(np.corrcoef(a, b)[0, 1])


# ------------------------------ slicing single notes out of the scale -----
def split_scale(x, n_notes=24, bpm=66.0, ticks_per_note=8, ticks_per_beat=4,
                start_midi=48):
    """Cut by score timing. Returns [(midi, samples of that note), ...]"""
    tick = 60.0 / bpm / ticks_per_beat
    dur = ticks_per_note * tick
    out = []
    for i in range(n_notes):
        a = int(i * dur * SR)
        b = int((i + 1) * dur * SR)
        if a >= len(x):
            break
        out.append((start_midi + i, x[a:min(b, len(x))]))
    return out


# ================================================================== main ====
def aperiodic(x, f0, t0=0.4, dur=1.2):
    """Aperiodic (noise) component: x[n] - x[n-T]. T is one fundamental period.
    Returns (fraction of the total energy, distribution % of the residual over 4 bands).

    Why measure it: a real instrument's breath noise, bow noise and string
    knocks all live here, but they sit at roughly -70 dB, where none of harmonic
    LSD, spectrogram MAE and centroid can see them. Measured on flute: the
    synthesis side's noise layer was once a full 27 dB short (an energy ratio
    used as an amplitude ratio), so it sounded like an organ rather than a
    flute, and not one of those metrics reacted -- a textbook blind spot.
    """
    k = int(0.01 * SR)
    e = np.array([np.sqrt(np.mean(x[i:i + k] ** 2)) for i in range(0, max(len(x) - k, 1), k)])
    if e.max() <= 0:
        return np.nan, None
    on = int(np.argmax(e > 0.08 * e.max())) * k
    seg = x[on + int(t0 * SR): on + int((t0 + dur) * SR)]
    T = int(round(SR / f0))
    if len(seg) < T + 4096:
        return np.nan, None
    n = (len(seg) - T) // 1024 * 1024
    dif = seg[T:T + n] - seg[:n]
    frac = float(np.sum(dif ** 2) / (2 * np.sum(seg[:n] ** 2) + 1e-15))
    m = np.abs(np.fft.rfft(dif * np.hanning(n))) ** 2
    fr = np.fft.rfftfreq(n, 1 / SR)
    bands = [(0, 900), (900, 2000), (2000, 5000), (5000, 22050)]
    dist = np.array([m[(fr >= lo) & (fr < hi)].sum() for lo, hi in bands])
    return frac, dist / max(dist.sum(), 1e-15) * 100.0


def compare_note(syn, ref, f0):
    """Return every metric for this note. Both sides are first aligned to their own onset."""
    es, er = envelope(syn), envelope(ref)
    os_, or_ = find_onset(es), find_onset(er)
    syn_a, ref_a = syn[os_ * int(0.005 * SR):], ref[or_ * int(0.005 * SR):]
    es_a, er_a = envelope(syn_a), envelope(ref_a)

    # A sustained instrument's recording (bowed strings, winds) ends with the
    # player stopping, but the synth plays for the MIDI note length and should
    # not stop by itself. Comparing the whole span gives the false conclusion
    # "the envelopes are completely uncorrelated" (measured violin r = -0.006,
    # while the spectrum is actually fine). So sustained types are compared only
    # up to the end of the reference material's body; only decaying types get the
    # whole span.
    body_end = len(er_a)
    hi = np.where(er_a >= 0.6 * er_a.max())[0]
    if len(hi) > 1:
        body_frac = (hi[-1] - hi[0]) / max(len(er_a), 1)
        if body_frac >= 0.25:                    # sustained
            body_end = int(hi[-1])

    n = min(len(es_a), len(er_a), body_end, int(2.0 / 0.005))
    if n < 20:
        return None
    # Compare the envelopes in the log domain: ears are logarithmic, and the linear domain is dominated by the attack
    ls = 20 * np.log10(es_a[:n] / max(es_a.max(), 1e-12) + 1e-6)
    lr = 20 * np.log10(er_a[:n] / max(er_a.max(), 1e-12) + 1e-6)

    # Once the reference envelope is flat enough, the correlation coefficient
    # measures noise, not shape.
    #
    # Measured log-envelope standard deviation inside the compared window:
    #     piano 7~10 dB     violin 12~26 dB      flute 2.0~2.1 dB     trumpet 2.1 dB
    # Flute and trumpet are nearly flat in that window (a sustained tone is
    # supposed to be flat), and the Pearson r of two near-horizontal curves is
    # decided entirely by the random wobble of vibrato and breath noise --
    # exactly what the synth neither should nor can reproduce point by point.
    # Measured on the same material with one completely unrelated parameter
    # changed, the flute's r jumps around anywhere between 0.0 and 0.9.
    #
    # So when it is too flat to measure shape, report "not measurable" rather
    # than handing back noise that looks like a score.
    # Threshold 3 dB: the four instruments above sit well clear of that line.
    env_r_val = pearson(ls, lr) if np.std(lr) >= ENV_FLAT_STD_DB else np.nan

    ds, dr = decay_time_db(es_a), decay_time_db(er_a)
    decay_cents = (1200 * np.log2(ds / dr)) if (ds and dr and not np.isnan(ds) and not np.isnan(dr)) else np.nan

    # Pin down f0 on each side before taking harmonics, or off-pitch playing throws the whole comparison out of place
    f0_ref = refine_f0(ref_a, f0, 0.3)
    f0_syn = refine_f0(syn_a, f0, 0.3)
    hl = [lsd(harmonic_dist(ref_a, f0_ref, t), harmonic_dist(syn_a, f0_syn, t))
          for t in (0.05, 0.3, 1.0)]

    Ms, Mr = mel_spectrogram(syn_a), mel_spectrogram(ref_a)
    k = min(len(Ms), len(Mr))
    # Normalise each side to its own maximum -- this compares "shape", not "loudness"
    Ms = Ms[:k] - Ms[:k].max()
    Mr = Mr[:k] - Mr[:k].max()
    # Floor at -30 dB (relative to the overall peak), not -60.
    #
    # Why it changed: real instruments contain random components by nature. A
    # control experiment was run -- add noise that is "exactly the right amount,
    # only a different random realisation" to the original file, then compare
    # that against the original file itself:
    #     floor -60 dB -> false difference 5.4 dB   piano vs violin (real) 14.7 dB
    #     floor -30 dB -> false difference 1.3 dB   piano vs violin          4.1 dB
    # With the floor too low, most of what the metric penalises is "the random
    # draw differs", which cannot and should not be fixed, and the discrimination
    # (real difference / false difference) actually gets worse. Content below
    # -30 dB is perceptually masked anyway.
    #
    # When reading it, remember: this metric's own noise floor is about 1.3 dB,
    # and differences below that mean nothing.
    mel_mae = float(np.mean(np.abs(np.maximum(Ms, MEL_FLOOR_DB) - np.maximum(Mr, MEL_FLOOR_DB))))

    cs, es_e = centroid_track(syn_a)
    cr, er_e = centroid_track(ref_a)
    k = min(len(cs), len(cr))
    cs, cr, er_e = cs[:k], cr[:k], er_e[:k]
    # Only compare the stretches where the reference is loud enough: in quiet parts the centroid is set by the noise floor and means nothing
    live = er_e > 0.01 * er_e.max()
    cent_r = pearson(cs[live], cr[live]) if live.sum() >= 4 else np.nan
    w = er_e / max(er_e.sum(), 1e-12)
    cent_syn = float(np.sum(cs * w))
    cent_ref = float(np.sum(cr * w))

    nf_s, nd_s = aperiodic(syn_a, f0)
    nf_r, nd_r = aperiodic(ref_a, f0)
    # amount error in dB (0 = exactly right; positive = too much noise)
    noise_db = (10 * np.log10((nf_s + 1e-9) / (nf_r + 1e-9))
                if (nf_s == nf_s and nf_r == nf_r) else np.nan)
    # placement error: which bands the residual falls in, as a mean absolute error in percentage points
    noise_pos = (float(np.mean(np.abs(nd_s - nd_r)))
                 if (nd_s is not None and nd_r is not None) else np.nan)

    return dict(noise_db=noise_db, noise_pos=noise_pos,
                env_r=env_r_val,
                decay_cents=decay_cents,
                lsd_atk=hl[0], lsd_mid=hl[1], lsd_late=hl[2],
                mel_mae=mel_mae,
                cent_r=cent_r, cent_syn=cent_syn, cent_ref=cent_ref,
                sustaining=bool(body_end < len(er_a)))


def main():
    ap = argparse.ArgumentParser(description="合成音色 vs 原始素材的客觀評測")
    ap.add_argument("scale", help="合成的半音階 WAV（韌體按 w 錄的 PLAY.WAV，或模擬器產生的）")
    ap.add_argument("refs", nargs="+", help="原始單音素材（檔案或資料夾）")
    ap.add_argument("--bpm", type=float, default=66.0)
    # Default is the firmware's 24 notes C3~B4; files produced by xrange cover a different range, use these two options
    ap.add_argument("--start", type=int, default=48,
                    help="合成檔第一個音的 MIDI 音高（預設 48 = C3）")
    ap.add_argument("--count", type=int, default=24,
                    help="合成檔有幾個音（預設 24）")
    ap.add_argument("--plot", default=None, help="輸出比較圖 PNG（需要 matplotlib）")
    ap.add_argument("--json", default=None,
                    help="把逐音的數字另存成 JSON，給 report_docx.py 產生 Word 報告用")
    args = ap.parse_args()

    # collect the reference material
    files = []
    for r in args.refs:
        # Accept either case of the extension: what the firmware writes is
        # REC.WAV / T_C4.WAV (all caps), and taking lowercase only silently
        # skips every "evaluate with material the machine recorded itself" run,
        # with an error message reading "no reference material could be paired
        # by pitch" that makes it look like a filename-format problem.
        if os.path.isdir(r):
            files += glob.glob(os.path.join(r, "*.wav")) + glob.glob(os.path.join(r, "*.WAV"))
        else:
            files += glob.glob(r)
    ref_by_midi = {}
    for f in files:
        m = midi_of_filename(f)
        if m is not None:
            ref_by_midi[m] = f
    if not ref_by_midi:
        print("找不到能配對音高的參考素材（檔名要含音名，例如 Piano.mf.Db4.wav）", file=sys.stderr)
        sys.exit(1)

    scale = read_wav(args.scale)
    notes = split_scale(scale, n_notes=args.count, bpm=args.bpm, start_midi=args.start)

    print(f"合成檔 {args.scale}  {len(scale)/SR:.1f} 秒，切出 {len(notes)} 個音")
    print(f"參考素材 {len(ref_by_midi)} 個：" +
          " ".join(midi_name(m) for m in sorted(ref_by_midi)))
    print()
    hdr = f"{'音':>5s} {'包絡r':>7s} {'衰減誤差':>9s} {'LSD起音':>8s} {'LSD中段':>8s} {'LSD尾段':>8s} {'頻譜圖':>7s} {'質心r':>7s} {'噪聲量':>7s} {'噪聲位置':>9s}"
    print(hdr)
    print("-" * len(hdr))

    rows = []
    for midi, seg in notes:
        if midi not in ref_by_midi:
            continue
        ref = read_wav(ref_by_midi[midi])
        f0 = 440.0 * 2 ** ((midi - 69) / 12)
        r = compare_note(seg, ref, f0)
        if r is None:
            continue
        r["midi"] = midi
        rows.append(r)
        print(f"{midi_name(midi):>5s} {r['env_r']:7.3f} {r['decay_cents']:8.0f}c "
              f"{r['lsd_atk']:8.1f} {r['lsd_mid']:8.1f} {r['lsd_late']:8.1f} "
              f"{r['mel_mae']:7.1f} {r['cent_r']:7.3f} {r['noise_db']:+6.1f}dB {r['noise_pos']:8.1f}pt")

    if not rows:
        print("\n沒有配對成功的音。檢查半音階的起始音是否為 C3、BPM 是否正確。")
        return

    def avg(k):
        v = [x[k] for x in rows if not np.isnan(x[k])]
        return float(np.mean(v)) if v else float("nan")

    print("-" * len(hdr))
    print(f"{'平均':>5s} {avg('env_r'):7.3f} {avg('decay_cents'):8.0f}c "
          f"{avg('lsd_atk'):8.1f} {avg('lsd_mid'):8.1f} {avg('lsd_late'):8.1f} "
          f"{avg('mel_mae'):7.1f} {avg('cent_r'):7.3f} {avg('noise_db'):+6.1f}dB {avg('noise_pos'):8.1f}pt")
    print(f"""
判讀標準
  包絡相關性 r   > 0.95 很好    0.85~0.95 可接受    < 0.85 包絡不對
  衰減時間誤差   < 200 cent（約 1.12 倍）算對，>600 cent 一聽就是不同樂器
  諧波 LSD       < 3 dB 很好    3~6 dB 可接受       > 8 dB 音色明顯不同
  頻譜圖 MAE     < 3 dB 很好    3~6 dB 可接受       > 9 dB 差很多
                 （本底約 1.3 dB：樂器本身的隨機成分無法逐點重現）
  質心相關性 r   > 0.9 亮度變化走向正確
  噪聲量         非週期成分的份量誤差；|x| < 3 dB 很好，> 10 dB 一聽就少了呼吸感
  噪聲位置       非週期成分落在哪些頻帶（4 帶百分點 MAE）；< 8 pt 很好
                 這兩欄專門補上其他指標的盲點：噪聲層約在 -70 dB，
                 LSD / 頻譜圖 / 質心都看不到它，但耳朵聽得出來""")

    if args.plot:
        try:
            make_plot(args.plot, rows, notes, ref_by_midi)
        except ImportError:
            print("\n(沒有 matplotlib，跳過畫圖：pip install matplotlib)")

    # Also write the per-note numbers out in a machine-readable form. The report
    # generator eats that file rather than parsing the table above -- the table
    # is for humans, its format changes whenever readability calls for it, and
    # using it as a data interface will eventually break silently after one
    # column-width edit.
    if args.json:
        import json
        out = {
            "scale_file": os.path.abspath(args.scale),
            "scale_seconds": round(len(scale) / SR, 2),
            "start_midi": args.start,
            "count": args.count,
            "bpm": args.bpm,
            "refs": {midi_name(m): os.path.abspath(p) for m, p in sorted(ref_by_midi.items())},
            "notes": [
                dict(note=midi_name(r["midi"]),
                     ref=os.path.basename(ref_by_midi[r["midi"]]),
                     **{k: (None if (isinstance(v, float) and np.isnan(v)) else
                            (float(v) if isinstance(v, (int, float, np.floating)) else v))
                        for k, v in r.items()})
                for r in rows
            ],
            "mean": {k: (None if np.isnan(avg(k)) else avg(k)) for k in METRIC_KEYS},
        }
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(out, f, ensure_ascii=False, indent=2)
        print(f"\n已輸出 JSON：{args.json}")


def make_plot(path, rows, notes, ref_by_midi):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    # Always label the plots in English. The default matplotlib font has no CJK
    # glyphs, so Chinese comes out as tofu boxes; showing it would mean
    # installing and registering a CJK font first, an extra burden on the user.
    # The terminal report is still full Chinese.

    # pick a note that has a reference and plot its spectrogram and envelope
    pick = None
    for midi, seg in notes:
        if midi in ref_by_midi:
            pick = (midi, seg)
            break
    fig, ax = plt.subplots(2, 2, figsize=(13, 8))

    midi, seg = pick
    ref = read_wav(ref_by_midi[midi])
    es, er = envelope(seg), envelope(ref)
    seg = seg[find_onset(es) * int(0.005 * SR):]
    ref = ref[find_onset(er) * int(0.005 * SR):]

    Ms, Mr = mel_spectrogram(seg), mel_spectrogram(ref)
    k = min(len(Ms), len(Mr))
    for a, M, t in ((ax[0][0], Mr[:k], f"REFERENCE  {midi_name(midi)}"),
                    (ax[0][1], Ms[:k], f"SYNTHESIZED  {midi_name(midi)}")):
        a.imshow((M - M.max()).T, aspect="auto", origin="lower", vmin=-60, vmax=0,
                 extent=[0, k * 512 / SR, 0, M.shape[1]])
        a.set_title(t); a.set_xlabel("time (s)"); a.set_ylabel("log-freq band")

    es, er = envelope(seg), envelope(ref)
    n = min(len(es), len(er), 400)
    t = np.arange(n) * 0.005
    ax[1][0].plot(t, 20*np.log10(er[:n]/er.max()+1e-6), label="reference")
    ax[1][0].plot(t, 20*np.log10(es[:n]/es.max()+1e-6), label="synthesized")
    ax[1][0].set_ylim(-60, 2); ax[1][0].legend(); ax[1][0].grid(alpha=.3)
    ax[1][0].set_title(f"envelope (dB)  {midi_name(midi)}"); ax[1][0].set_xlabel("time (s)")

    ms = [r["midi"] for r in rows]
    ax[1][1].plot(ms, [r["lsd_mid"] for r in rows], "o-", label="harmonic LSD (dB)")
    ax[1][1].plot(ms, [r["mel_mae"] for r in rows], "s-", label="spectrogram MAE (dB)")
    ax2 = ax[1][1].twinx()
    ax2.plot(ms, [r["env_r"] for r in rows], "^--", color="g", label="envelope r")
    ax2.set_ylim(0, 1.05); ax2.set_ylabel("correlation")
    ax[1][1].set_xticks(ms); ax[1][1].set_xticklabels([midi_name(m) for m in ms], rotation=90)
    ax[1][1].legend(loc="upper left"); ax2.legend(loc="upper right")
    ax[1][1].grid(alpha=.3); ax[1][1].set_title("per-note metrics")
    fig.suptitle("TimbreClone  synthesized vs reference", fontsize=13)

    plt.tight_layout()
    plt.savefig(path, dpi=110)
    print(f"\n已輸出比較圖：{path}")


if __name__ == "__main__":
    main()

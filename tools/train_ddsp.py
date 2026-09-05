#!/usr/bin/env python3
# =============================================================================
#  train_ddsp.py  -  trains TimbreClone's little MLP and writes MODEL.BIN
#
#  Dependencies: numpy only. (WAV read with the stdlib wave module, backprop hand-rolled.)
#
#  Usage
#  -----
#    # The common case: just feed it a few monophonic WAVs (same instrument, the more pitches the better)
#    python3 train_ddsp.py violin_a3.wav violin_c5.wav violin_e4.wav -o MODEL.BIN
#
#    # Or take the per-frame analysis file exported by pressing 'c' on the Teensy
#    python3 train_ddsp.py --csv FRAMES.CSV -o MODEL.BIN
#
#  Copy MODEL.BIN to the root of the SD card and the Teensy loads it automatically at boot.
#
#  Model
#  -----
#    4 inputs: [ log2(f0/261.63)/PITCH_SCALE , loudness , normalized time since attack , released ]
#    32 -> 32 (tanh) -> 33
#    the first 32 logits go through softmax = partial distribution; the 33rd through sigmoid = noise ratio
#    loss = cross-entropy(partial distribution) + 0.3 * BCE(noise)
#
#  Why can it be this small?
#    The formant shift caused by pitch is already dealt with on the Teensy side by the
#    spectral envelope, so the MLP only has to learn the timbre changes caused by
#    loudness / time / register, and those are low-dimensional and smooth.
# =============================================================================

import argparse
import struct
import sys
import wave

import numpy as np

# --------------------------------------------------------------- Params -----
SR        = 44100
NFFT      = 2048
HOP       = 512
N_HARM    = 32                  # Must match TC_N_HARM in config.h
MLP_IN    = 4
MLP_H1    = 32
MLP_H2    = 32
MLP_OUT   = N_HARM + 1
MAGIC     = 0x324D4C50          # Must match TC_MLP_MAGIC in config.h
PITCH_SCALE = 1.0               # Must match TC_MLP_PITCH_SCALE in config.h
F0_MIN    = 65.0
F0_MAX    = 1500.0
TIME_WARP_TAU = 0.06            # Must match TC_TIME_WARP_TAU in config.h


def time_warp(t, note_dur):
    """Log time axis. The attack lasts only tens of ms yet has to share keyframes with seconds
    of sustain; a linear split leaves it completely unmodelled. Must match config.h's tc_timeWarp exactly."""
    note_dur = max(note_dur, 1e-3)
    k = 1.0 / TIME_WARP_TAU
    return np.log1p(k * np.maximum(t, 0.0)) / np.log1p(k * note_dur)


# ============================================================== WAV read =====
def read_wav(path):
    with wave.open(path, "rb") as w:
        nch, sw, sr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if sw != 2:
        raise ValueError(f"{path}: 只支援 16-bit PCM WAV")
    x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    if nch > 1:
        x = x.reshape(-1, nch).mean(axis=1)
    if sr != SR:                                   # Linear resampling is good enough
        t_old = np.arange(len(x)) / sr
        t_new = np.arange(0, t_old[-1], 1.0 / SR)
        x = np.interp(t_new, t_old, x)
    return x


# ================================================================ YIN ========
def yin_f0(frame, sr=SR, thresh=0.15):
    """Estimate f0 over an NFFT-long frame, using an FFT to speed up the difference function."""
    W = len(frame) // 2
    tau_max = min(int(sr / F0_MIN), W - 1)
    tau_min = max(int(sr / F0_MAX), 2)

    x = frame[:2 * W]
    # d(tau) = p(0) + p_tau - 2*r(tau)
    nfft = 1 << (2 * W - 1).bit_length()
    f = np.fft.rfft(x, nfft)
    r = np.fft.irfft(f * np.conj(f), nfft)[:W]

    cum = np.concatenate(([0.0], np.cumsum(x ** 2)))
    p0 = cum[W] - cum[0]
    ptau = cum[W + np.arange(W)] - cum[np.arange(W)]
    d = p0 + ptau - 2 * r
    d[0] = 0.0

    run = np.cumsum(d[1:tau_max + 1])
    dn = np.ones(tau_max + 1)
    idx = np.arange(1, tau_max + 1)
    with np.errstate(divide="ignore", invalid="ignore"):
        dn[1:] = np.where(run > 1e-12, d[1:tau_max + 1] * idx / run, 1.0)

    tau = -1
    for t in range(tau_min, tau_max):
        if dn[t] < thresh:
            while t + 1 < tau_max and dn[t + 1] < dn[t]:
                t += 1
            tau = t
            break
    if tau < 0:
        seg = dn[tau_min:tau_max]
        if len(seg) == 0:
            return 0.0
        tau = tau_min + int(np.argmin(seg))
        if dn[tau] > 0.6:
            return 0.0

    if 0 < tau < tau_max - 1:                      # Parabolic interpolation
        a, b, c = dn[tau - 1], dn[tau], dn[tau + 1]
        den = 2 * (2 * b - a - c)
        if abs(den) > 1e-12:
            tau = tau + (c - a) / den
    return sr / tau


# ===================================================== Frame analysis ========
def analyze_file(path, verbose=True):
    """Returns (X, Yh, Yn): input features, partial-distribution targets, noise targets."""
    x = read_wav(path)
    if len(x) < NFFT * 2:
        raise ValueError(f"{path}: 太短")

    win = np.hanning(NFFT)
    n_frames = (len(x) - NFFT) // HOP + 1

    # --- RMS envelope / onset / offset ---
    rms = np.array([np.sqrt(np.mean(x[i * HOP:i * HOP + HOP] ** 2)) for i in range(n_frames)])
    if rms.max() < 1e-4:
        raise ValueError(f"{path}: 幾乎是靜音")
    rms = rms / rms.max()

    onset = int(np.argmax(rms > 0.08))
    tail = np.where(rms > 0.04)[0]
    offset = int(tail[-1]) if len(tail) else n_frames - 1
    if offset - onset < 6:
        raise ValueError(f"{path}: 有效音長太短")

    # --- f0 (take the median) ---
    peak = onset + int(np.argmax(rms[onset:offset + 1]))
    cands = []
    for k in range(9):
        f = peak + int((offset - peak) * k / 9)
        if f * HOP + NFFT > len(x):
            break
        p = yin_f0(x[f * HOP:f * HOP + NFFT])
        if F0_MIN < p < F0_MAX:
            cands.append(p)
    if not cands:
        raise ValueError(f"{path}: 抓不到基頻")
    f0 = float(np.median(cands))

    dur = (offset - onset) * HOP / SR
    rel_start = onset + int((offset - onset) * 0.80)     # Treat the tail as release

    X, Yh, Yn = [], [], []
    bin_hz = SR / NFFT
    for f in range(onset, offset + 1):
        seg = x[f * HOP:f * HOP + NFFT]
        if len(seg) < NFFT:
            break
        mag = np.abs(np.fft.rfft(seg * win))
        total = float(np.sum(mag ** 2))

        amp = np.zeros(N_HARM)
        harm_e = 0.0
        for h in range(N_HARM):
            fh = (h + 1) * f0
            if fh > SR * 0.48:
                continue
            c = int(round(fh / bin_hz))
            if c < 2 or c > len(mag) - 3:
                continue
            b = c + int(np.argmax(mag[c - 2:c + 3])) - 2
            a_, b_, c_ = mag[b - 1], mag[b], mag[b + 1]
            den = a_ - 2 * b_ + c_
            d = 0.5 * (a_ - c_) / den if abs(den) > 1e-12 else 0.0
            d = float(np.clip(d, -1, 1))
            amp[h] = max(b_ - 0.25 * (a_ - c_) * d, 0.0)
            harm_e += b_ ** 2 * 3.0

        s = amp.sum()
        if s < 1e-9:
            continue
        amp /= s
        noise = float(np.clip(1.0 - harm_e / total, 0.0, 0.9)) if total > 1e-12 else 0.0

        t_sec  = (f - onset) * HOP / SR
        t_norm = time_warp(t_sec, dur)
        X.append([np.clip(np.log2(f0 / 261.63) / PITCH_SCALE, -3.0, 3.0),
                  float(rms[f]),
                  float(np.clip(t_norm, 0.0, 1.5)),
                  1.0 if f >= rel_start else 0.0])
        Yh.append(amp)
        Yn.append(noise)

    if verbose:
        print(f"  {path}: f0={f0:7.2f} Hz  {len(X):4d} 格  音長 {dur:.2f}s  噪聲比 {np.mean(Yn):.3f}")
    return np.array(X), np.array(Yh), np.array(Yn)


def load_csv(path, verbose=True):
    """Reads the FRAMES.CSV exported by the Teensy: t,f0,loud,h1..h16,noise"""
    rows = np.genfromtxt(path, delimiter=",", skip_header=1)
    if rows.ndim == 1:
        rows = rows[None, :]
    t, f0, loud = rows[:, 0], rows[:, 1], rows[:, 2]
    harm = rows[:, 3:3 + N_HARM]
    noise = rows[:, 3 + N_HARM]

    dur = max(t.max(), 1e-3)
    rel = (t / dur > 0.80).astype(float)
    X = np.stack([np.clip(np.log2(f0 / 261.63) / PITCH_SCALE, -3.0, 3.0),
                  loud,
                  np.clip(time_warp(t, dur), 0.0, 1.5),
                  rel], axis=1)
    harm = harm / np.maximum(harm.sum(axis=1, keepdims=True), 1e-9)
    if verbose:
        print(f"  {path}: {len(X)} 格 (CSV)")
    return X, harm, noise


# ================================================================ MLP ========
def init_params(rng):
    def he(fan_in, shape):
        return rng.normal(0, np.sqrt(1.0 / fan_in), shape)
    return {
        "w1": he(MLP_IN, (MLP_H1, MLP_IN)),  "b1": np.zeros(MLP_H1),
        "w2": he(MLP_H1, (MLP_H2, MLP_H1)),  "b2": np.zeros(MLP_H2),
        "w3": he(MLP_H2, (MLP_OUT, MLP_H2)), "b3": np.zeros(MLP_OUT),
    }


def forward(p, X):
    z1 = X @ p["w1"].T + p["b1"];  a1 = np.tanh(z1)
    z2 = a1 @ p["w2"].T + p["b2"]; a2 = np.tanh(z2)
    z3 = a2 @ p["w3"].T + p["b3"]
    return z1, a1, z2, a2, z3


def softmax(z):
    z = z - z.max(axis=1, keepdims=True)
    e = np.exp(z)
    return e / e.sum(axis=1, keepdims=True)


def train(X, Yh, Yn, epochs=4000, lr=3e-3, lam=0.3, seed=0, verbose=True):
    rng = np.random.default_rng(seed)
    p = init_params(rng)
    m = {k: np.zeros_like(v) for k, v in p.items()}
    v = {k: np.zeros_like(val) for k, val in p.items()}
    b1_, b2_, eps = 0.9, 0.999, 1e-8
    N = len(X)
    batch = min(256, N)

    for ep in range(1, epochs + 1):
        idx = rng.choice(N, batch, replace=False)
        xb, yhb, ynb = X[idx], Yh[idx], Yn[idx]

        z1, a1, z2, a2, z3 = forward(p, xb)
        ph = softmax(z3[:, :N_HARM])
        pn = 1.0 / (1.0 + np.exp(-z3[:, N_HARM]))

        # Gradients: the logit gradients of softmax+CE and sigmoid+BCE are both clean
        dz3 = np.zeros_like(z3)
        dz3[:, :N_HARM] = (ph - yhb) / batch
        dz3[:, N_HARM] = lam * (pn - ynb) / batch

        g = {}
        g["w3"] = dz3.T @ a2;          g["b3"] = dz3.sum(axis=0)
        da2 = dz3 @ p["w3"];           dz2 = da2 * (1 - a2 ** 2)
        g["w2"] = dz2.T @ a1;          g["b2"] = dz2.sum(axis=0)
        da1 = dz2 @ p["w2"];           dz1 = da1 * (1 - a1 ** 2)
        g["w1"] = dz1.T @ xb;          g["b1"] = dz1.sum(axis=0)

        for k in p:                                   # Adam
            m[k] = b1_ * m[k] + (1 - b1_) * g[k]
            v[k] = b2_ * v[k] + (1 - b2_) * g[k] ** 2
            mh = m[k] / (1 - b1_ ** ep)
            vh = v[k] / (1 - b2_ ** ep)
            p[k] -= lr * mh / (np.sqrt(vh) + eps)

        if verbose and (ep % 500 == 0 or ep == 1):
            _, _, _, _, z = forward(p, X)
            ph_all = softmax(z[:, :N_HARM])
            ce = -np.mean(np.sum(Yh * np.log(ph_all + 1e-9), axis=1))
            l1 = np.mean(np.abs(ph_all - Yh))
            print(f"    epoch {ep:5d}   CE {ce:.4f}   平均諧波誤差 {l1:.5f}")
    return p


# ============================================================ Export =========
def export(p, path):
    """Must line up exactly with the MlpWeights memory layout in timbre_model.h."""
    with open(path, "wb") as f:
        f.write(struct.pack("<I", MAGIC))
        for key, shape in (("w1", (MLP_H1, MLP_IN)), ("b1", (MLP_H1,)),
                           ("w2", (MLP_H2, MLP_H1)), ("b2", (MLP_H2,)),
                           ("w3", (MLP_OUT, MLP_H2)), ("b3", (MLP_OUT,))):
            arr = np.asarray(p[key], dtype="<f4")
            assert arr.shape == shape, f"{key} 形狀錯誤 {arr.shape} != {shape}"
            f.write(arr.tobytes(order="C"))
    size = 4 + (MLP_H1 * MLP_IN + MLP_H1 + MLP_H2 * MLP_H1 + MLP_H2
                + MLP_OUT * MLP_H2 + MLP_OUT) * 4
    import os
    actual = os.path.getsize(path)
    assert actual == size, f"檔案大小 {actual} != 預期 {size}"
    print(f"\n[OK] 已寫出 {path}  ({actual} bytes)")
    print("     把它複製到 SD 卡根目錄，Teensy 開機或按 'm' 就會載入。")


# ================================================================ main =======
def main():
    ap = argparse.ArgumentParser(description="訓練 TimbreClone 的 DDSP-lite MLP")
    ap.add_argument("wavs", nargs="*", help="單音 WAV 檔（同一把樂器，不同音高越多越好）")
    ap.add_argument("--csv", action="append", default=[], help="Teensy 匯出的 FRAMES.CSV")
    ap.add_argument("-o", "--out", default="MODEL.BIN")
    ap.add_argument("--epochs", type=int, default=4000)
    ap.add_argument("--lr", type=float, default=3e-3)
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    if not args.wavs and not args.csv:
        ap.error("至少要給一個 WAV 或 --csv")

    print("讀取素材：")
    Xs, Yhs, Yns = [], [], []
    for w in args.wavs:
        try:
            X, Yh, Yn = analyze_file(w)
            Xs.append(X); Yhs.append(Yh); Yns.append(Yn)
        except Exception as e:
            print(f"  ! 跳過 {w}: {e}", file=sys.stderr)
    for c in args.csv:
        X, Yh, Yn = load_csv(c)
        Xs.append(X); Yhs.append(Yh); Yns.append(Yn)

    if not Xs:
        print("沒有可用的資料", file=sys.stderr)
        sys.exit(1)

    X  = np.concatenate(Xs)
    Yh = np.concatenate(Yhs)
    Yn = np.concatenate(Yns)
    print(f"\n總共 {len(X)} 筆訓練樣本，涵蓋 {len(set(np.round(X[:,0],3)))} 個不同音高")
    if len(set(np.round(X[:, 0], 3))) < 2:
        print("提醒：只有一個音高。MLP 只會學到「時間 / 響度」對音色的影響，")
        print("      跨音高的音色變化由 Teensy 端的頻譜包絡校正負責。")
        print("      想要更好的結果，多錄幾個不同音高的單音再訓練一次。")

    print("\n開始訓練：")
    p = train(X, Yh, Yn, epochs=args.epochs, lr=args.lr, seed=args.seed)

    # Sample a few points to see what the model has learned
    print("\n訓練後的諧波分佈抽樣（起音 / 中段 / 尾段）：")
    for tn, label in ((0.05, "起音"), (0.50, "中段"), (0.95, "尾段")):
        probe = np.array([[float(np.median(X[:, 0])), 0.8, tn, 1.0 if tn > 0.8 else 0.0]])
        _, _, _, _, z = forward(p, probe)
        ph = softmax(z[:, :N_HARM])[0]
        print(f"  {label}: " + " ".join(f"{v:.3f}" for v in ph[:8]) + " ...")

    export(p, args.out)


if __name__ == "__main__":
    main()

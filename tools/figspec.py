#!/usr/bin/env python3
# 產生「目標 vs 輸出」頻譜圖比較表（Timbre Shadowing 圖 9~13 的同一種版式）
import sys, wave, numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter
from scipy.signal import spectrogram

FS_TARGET = 44100

def load(path):
    w = wave.open(path); sr = w.getframerate(); nch = w.getnchannels()
    d = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64)
    w.close()
    if nch == 2: d = d.reshape(-1, 2).mean(1)
    return d / 32768.0, sr

def panel(ax, cax, x, sr, title, tmax):
    f, t, S = spectrogram(x, fs=sr, window="hann", nperseg=1024,
                          noverlap=768, mode="magnitude")
    ref = np.max(S) if np.max(S) > 0 else 1.0
    D = 20*np.log10(np.maximum(S/ref, 1e-9))
    m = ax.pcolormesh(t, f, D, cmap="magma", vmin=-80, vmax=0, shading="gouraud")
    ax.set_ylim(0, 8000); ax.set_xlim(0, tmax)
    ax.set_title(title, fontsize=8, pad=4)
    ax.set_xlabel("Time", fontsize=6)
    ax.set_ylabel("Hz", fontsize=6)
    ax.set_xticks(np.arange(0, tmax + 1e-9, 0.25))
    ax.xaxis.set_major_formatter(FuncFormatter(
        lambda v, p: ("%g" % round(v, 2))))
    ax.tick_params(labelsize=6, length=2, pad=1)
    cb = plt.colorbar(m, cax=cax, ticks=range(-80, 1, 10))
    cb.ax.set_yticklabels([f"{v:+d} dB".replace("+0 dB", "+0 dB") for v in range(-80, 1, 10)],
                          fontsize=6)
    cb.ax.tick_params(length=2, pad=1)
    cb.outline.set_linewidth(0.4)

def build(rows, out_png, tmax=2.0):
    """rows: [(label, ref_wav, syn_wav), ...] — 由上而下"""
    n = len(rows)
    ROW_IN = 3.50                      # 每列高度（英吋）
    fig = plt.figure(figsize=(9.60, ROW_IN*n), dpi=100)
    for i, (label, rp, sp) in enumerate(rows):
        band_top = 1.0 - i/n           # 這一列的上緣（figure 座標）
        lab_y    = band_top - 0.012/n*3.5
        b        = band_top - (0.80/n) # 圖框底
        h        = 0.58/n              # 圖框高
        fig.text(0.012, lab_y, label, fontsize=7.5, va="top", ha="left")
        axL = fig.add_axes([0.055, b, 0.350, h])
        cL  = fig.add_axes([0.419, b, 0.011, h])
        axR = fig.add_axes([0.545, b, 0.350, h])
        cR  = fig.add_axes([0.909, b, 0.011, h])
        xr, sr1 = load(rp); xs, sr2 = load(sp)
        panel(axL, cL, xr, sr1, "Target Spectrogram", tmax)
        panel(axR, cR, xs, sr2, "Output Spectrogram", tmax)
    fig.savefig(out_png, dpi=100, facecolor="white")
    plt.close(fig)
    print("寫出", out_png)

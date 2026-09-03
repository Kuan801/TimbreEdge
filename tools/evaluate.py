#!/usr/bin/env python3
# =============================================================================
#  evaluate.py  -  客觀比較「合成出來的半音階」與「原始單音素材」
#
#  用法
#  -----
#    python3 evaluate.py SCALE.WAV  /path/to/Piano.mf.*.wav
#    python3 evaluate.py SCALE.WAV  samples/ --plot report.png --json report.json
#
#  SCALE.WAV 是韌體演奏半音階錄下來的成品（按 w 存成 PLAY.WAV，或用模擬器產生）。
#  素材檔名要能看得出音名（C4 / Db4 / A4 ...），程式會自動配對。
#
#  音域從 C3~B4 寫死改成「音色庫範圍 + 一個八度」之後，--start 要跟著給：
#  素材是 C4~B4 的話，合成檔是 C4~B5，所以 --start 60 --count 24。
#
#  五個指標，全部都是「越小越好」除了相關性
#  -----------------------------------------
#   1. 包絡相關性      Pearson r，對數域比較（人耳是對數的）
#   2. 衰減時間誤差    掉到 -20 dB 所需時間的比值，用 cent 表示
#   3. 諧波分佈 LSD    對數頻譜距離（dB），取 -45 dB 底線避免被零值拉爆
#   4. 頻譜圖距離      mel 頻帶 × 時間的平均絕對差（dB）
#   5. 質心軌跡相關性  亮度隨時間變化的走向對不對
#
#  為什麼要有底線與對齊：
#    直接算 LSD 會被「真值為 0 的高次諧波」拉到上百 dB；
#    不對齊起音點的話所有時間相關的指標都會失真。
# =============================================================================

import argparse
import glob
import os
import re
import sys
import wave

import numpy as np

SR = 44100

# 參考包絡的對數標準差低於這個值，就不報包絡相關性（見 compare_note 的說明）
ENV_FLAT_STD_DB = 3.0
NOTE_NAMES = ["C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"]
ALT = {"C#": "Db", "D#": "Eb", "F#": "Gb", "G#": "Ab", "A#": "Bb"}


# =============================================================================
#  判讀標準
#
#  報告產生器（report_docx.py）直接 import 這份表，不要各自抄一份。
#  抄兩份的下場是「改了門檻卻只改一邊」，而報告上的顏色與結論仍然照舊 ——
#  那種錯不會有任何跡象，只會讓報告悄悄變成錯的。
#
#    key       -> (顯示名, 格式化函式, 很好, 可接受, 一句話說明)
#  「很好」與「可接受」都是 lambda，回傳 True 代表落在該區間。
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
    """回傳 'good' / 'ok' / 'bad' / 'na'。"""
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
    """從 Piano.mf.Db4.wav 這種檔名抓出 MIDI 音高。"""
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


# ------------------------------------------------------------ 基本量測 -----
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
    """從峰值掉 db 分貝所需的秒數（hop 0.005s）。掉不到就回 nan。"""
    if e.max() <= 0:
        return np.nan
    pk = int(np.argmax(e))
    target = e[pk] * (10 ** (-db / 20))
    for i in range(pk, len(e)):
        if e[i] <= target:
            return (i - pk) * 0.005
    return np.nan


def refine_f0(x, f0_nom, t0, N=8192, tol=0.06):
    """用訊號自己的頻譜把 f0 找準，不要直接信十二平均律的理論值。

    為什麼需要：真實演奏會偏音。實測小號 B5 的錄音基頻是 1006.5 Hz，比
    十二平均律的 987.8 高 32 音分。harmonic_dist 原本用 h*f0_nom 去取樣，
    搜尋視窗只有 +-27 Hz，到第 2 諧波就已經偏出 37 Hz —— 量到的是諧波之間
    的谷底而不是峰值，整條諧波分佈全錯。這會讓「高音合成得很差」這個結論
    根本站不住腳（實際上錯的是尺）。

    作法：在 f0_nom 的 +-6% 內找最強的峰，再用拋物線內插取得次格精度。
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
    # 拋物線內插，取得次格精度
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
    """對數頻譜距離。兩邊都壓底線，否則『真值為 0』的諧波會把指標拉到上百 dB。"""
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
    # log 頻率三角濾波器組
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
    """回傳 (質心軌跡, 每格能量)。能量要一起回傳，因為質心必須用能量加權——
    安靜的片段裡任何噪聲底都會把質心推很高。實測小提琴漸強素材，
    等權平均會算出 1433 Hz，能量加權後是 414 Hz，差 3.5 倍。"""
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


# --------------------------------------------------- 從半音階切出單音 -----
def split_scale(x, n_notes=24, bpm=66.0, ticks_per_note=8, ticks_per_beat=4,
                start_midi=48):
    """依樂譜時間切開。回傳 [(midi, 該音的樣本), ...]"""
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


# ==================================================================== 主 ====
def aperiodic(x, f0, t0=0.4, dur=1.2):
    """非週期（噪聲）成分：x[n] - x[n-T]。T 是一個基頻週期。
    回傳 (佔總能量的比例, 殘差在 4 個頻帶的分佈%)。

    為什麼要量這個：真實樂器的「氣聲/弓噪/擊弦雜音」全在這裡，可是它的
    位階大約在 -70 dB，諧波 LSD、頻譜圖 MAE、質心這三個指標全都看不到它。
    實測長笛：合成端的噪聲層曾經整整少了 27 dB（把能量比誤當振幅比用），
    聽起來像管風琴而不是長笛，但上述指標一個都沒有反應 —— 那是標準的盲點。
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
    """回傳這個音的所有指標。兩邊都先對齊到各自的起音點。"""
    es, er = envelope(syn), envelope(ref)
    os_, or_ = find_onset(es), find_onset(er)
    syn_a, ref_a = syn[os_ * int(0.005 * SR):], ref[or_ * int(0.005 * SR):]
    es_a, er_a = envelope(syn_a), envelope(ref_a)

    # 持續型樂器（弓弦/管樂）的錄音尾端含有「演奏者停止」這個動作，
    # 但合成器是照 MIDI 音長演奏、不該自己收掉。硬比整段會得到
    # 「包絡完全不相關」的假結論（實測小提琴 r = -0.006，其實頻譜好得很）。
    # 所以持續型只比到參考素材的本體結束為止；衰減型才比整段。
    body_end = len(er_a)
    hi = np.where(er_a >= 0.6 * er_a.max())[0]
    if len(hi) > 1:
        body_frac = (hi[-1] - hi[0]) / max(len(er_a), 1)
        if body_frac >= 0.25:                    # 持續型
            body_end = int(hi[-1])

    n = min(len(es_a), len(er_a), body_end, int(2.0 / 0.005))
    if n < 20:
        return None
    # 對數域比較包絡：人耳是對數的，線性域會被起音那一下主導
    ls = 20 * np.log10(es_a[:n] / max(es_a.max(), 1e-12) + 1e-6)
    lr = 20 * np.log10(er_a[:n] / max(er_a.max(), 1e-12) + 1e-6)

    # 參考包絡平到一定程度時，相關係數量的是雜訊，不是形狀。
    #
    # 實測被比對的那段窗內，對數包絡的標準差：
    #     鋼琴 7~10 dB      小提琴 12~26 dB      長笛 2.0~2.1 dB      小號 2.1 dB
    # 長笛與小號在這段窗裡幾乎是平的（持續音本來就該平），兩條近乎水平的
    # 曲線算 Pearson r，結果完全由顫音與呼吸噪音的隨機起伏決定 ——
    # 那正是合成器不該也無法逐點重現的東西。實測同一份素材、只換一個
    # 完全無關的參數，長笛的 r 會在 0.0 到 0.9 之間亂跳。
    #
    # 所以平到量不出形狀時就回報「量不到」，不要給一個看起來像分數的雜訊。
    # 門檻 3 dB：上面四種樂器在這條線兩側分得很開。
    env_r_val = pearson(ls, lr) if np.std(lr) >= ENV_FLAT_STD_DB else np.nan

    ds, dr = decay_time_db(es_a), decay_time_db(er_a)
    decay_cents = (1200 * np.log2(ds / dr)) if (ds and dr and not np.isnan(ds) and not np.isnan(dr)) else np.nan

    # 兩邊各自把 f0 找準再取諧波，否則演奏偏音會讓比對整個錯位
    f0_ref = refine_f0(ref_a, f0, 0.3)
    f0_syn = refine_f0(syn_a, f0, 0.3)
    hl = [lsd(harmonic_dist(ref_a, f0_ref, t), harmonic_dist(syn_a, f0_syn, t))
          for t in (0.05, 0.3, 1.0)]

    Ms, Mr = mel_spectrogram(syn_a), mel_spectrogram(ref_a)
    k = min(len(Ms), len(Mr))
    # 兩邊各自對齊到自己的最大值，比的是「形狀」不是「音量」
    Ms = Ms[:k] - Ms[:k].max()
    Mr = Mr[:k] - Mr[:k].max()
    # 底線設在 -30 dB（相對整段峰值），不是 -60。
    #
    # 為什麼要改：真實樂器本來就含有隨機成分。做過一個對照實驗 —— 把「份量
    # 完全正確、只是亂數實現不同」的噪聲加到原音檔上，再拿它跟原音檔自己比：
    #     底線 -60 dB -> 假差異 5.4 dB      鋼琴 vs 提琴（真差異）14.7 dB
    #     底線 -30 dB -> 假差異 1.3 dB      鋼琴 vs 提琴          4.1 dB
    # 底線壓太低時，指標大半在罰「亂數不一樣」這件無法也不該修的事，
    # 分辨力（真差異/假差異）反而更差。-30 dB 以下的成分在感知上也已被遮蔽。
    #
    # 判讀時請記得：這個指標的本底約 1.3 dB，低於它的差距沒有意義。
    mel_mae = float(np.mean(np.abs(np.maximum(Ms, MEL_FLOOR_DB) - np.maximum(Mr, MEL_FLOOR_DB))))

    cs, es_e = centroid_track(syn_a)
    cr, er_e = centroid_track(ref_a)
    k = min(len(cs), len(cr))
    cs, cr, er_e = cs[:k], cr[:k], er_e[:k]
    # 只比參考素材「夠大聲」的片段：安靜處的質心是噪聲底決定的，不具意義
    live = er_e > 0.01 * er_e.max()
    cent_r = pearson(cs[live], cr[live]) if live.sum() >= 4 else np.nan
    w = er_e / max(er_e.sum(), 1e-12)
    cent_syn = float(np.sum(cs * w))
    cent_ref = float(np.sum(cr * w))

    nf_s, nd_s = aperiodic(syn_a, f0)
    nf_r, nd_r = aperiodic(ref_a, f0)
    # 份量誤差用 dB（0 = 完全正確；正值 = 噪聲太多）
    noise_db = (10 * np.log10((nf_s + 1e-9) / (nf_r + 1e-9))
                if (nf_s == nf_s and nf_r == nf_r) else np.nan)
    # 位置誤差：殘差落在哪些頻帶，用百分點的平均絕對誤差
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
    # 預設是韌體的 C3~B4 24 音；xrange 產生的檔案音域不同，用這兩個參數指定
    ap.add_argument("--start", type=int, default=48,
                    help="合成檔第一個音的 MIDI 音高（預設 48 = C3）")
    ap.add_argument("--count", type=int, default=24,
                    help="合成檔有幾個音（預設 24）")
    ap.add_argument("--plot", default=None, help="輸出比較圖 PNG（需要 matplotlib）")
    ap.add_argument("--json", default=None,
                    help="把逐音的數字另存成 JSON，給 report_docx.py 產生 Word 報告用")
    args = ap.parse_args()

    # 收集參考素材
    files = []
    for r in args.refs:
        # 副檔名大小寫都要收：韌體自己寫出來的是 REC.WAV / T_C4.WAV（全大寫），
        # 只收小寫會讓「用機器自己錄的素材評測」整批被靜默略過，而且錯誤訊息
        # 是「找不到能配對音高的參考素材」，看起來像檔名格式不對。
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

    # 逐音的數字另存一份機器可讀的。報告產生器吃這個檔，
    # 而不是去解析上面那張表 —— 表格是給人看的，格式隨時會為了好讀而改，
    # 拿它當資料介面遲早會在改一次欄寬之後靜靜地壞掉。
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

    # 圖上一律用英文標籤。matplotlib 預設字型沒有 CJK 字符，中文會全部
    # 變成豆腐方塊；要顯示中文得先安裝並註冊 CJK 字型，對使用者是額外負擔。
    # 終端機的報表仍然是完整中文。

    # 挑一個有參考的音畫頻譜圖與包絡
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

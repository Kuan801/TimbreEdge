#!/usr/bin/env python3
# =============================================================================
#  bench.py  -  multi-instrument regression benchmark
#
#  Usage
#  -----
#    python3 bench.py --lib /path/to/sound_lib --save baseline.json
#    python3 bench.py --lib /path/to/sound_lib --compare baseline.json
#
#  Why this exists: when changing the synthesis code, the dangerous case is not
#  "nothing got better", it is "one instrument got better while another quietly
#  got worse". Looking at one instrument's numbers will never reveal that.
#
#  This runs the whole pipeline once per instrument (analyze -> synthesize a
#  chromatic scale -> compare note by note) and lays the means of nine metrics
#  out in a single table. --compare diffs every item against a previously saved
#  baseline, marks improvements with + and regressions with -, and says so
#  explicitly if any item falls back by more than the noise.
#
#  The "noise" is not guesswork: the same code run twice gives identical results
#  (synthesis has no randomness, and training is not involved), so any non-zero
#  difference was caused by the code. The threshold is only there to filter
#  floating-point rounding.
# =============================================================================

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from evaluate import CRITERIA, METRIC_KEYS, NOTE_NAMES, ALT   # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.join(HERE, "sim", "sim")
EVAL = os.path.join(HERE, "evaluate.py")

# Any difference beyond floating-point rounding counts. 1e-4 only filters the last digit's jitter.
EPS = 1e-4

# Which metrics are "bigger is better"
HIGHER_BETTER = {"env_r", "cent_r"}
# These metrics are ideally 0, so compare their absolute values
ABS_METRICS = {"decay_cents", "noise_db"}


def midi_of(path):
    m = re.search(r"\.([A-G][b#]?)(-?\d)\.", os.path.basename(path))
    if not m:
        m = re.search(r"([A-G][b#]?)(-?\d)", os.path.basename(path))
    if not m:
        return None
    name = ALT.get(m.group(1), m.group(1))
    if name not in NOTE_NAMES:
        return None
    return 12 * (int(m.group(2)) + 1) + NOTE_NAMES.index(name)


def run_one(name, wavs, workdir, verbose=False):
    """Run one instrument, return the mean dict (None on failure)."""
    midis = sorted(x for x in (midi_of(w) for w in wavs) if x is not None)
    if not midis:
        print(f"  {name}: 檔名看不出音高，跳過")
        return None

    out = os.path.join(workdir, f"{name}.wav")
    # The sim's SD emulation joins relative paths onto the root dir, so run inside the material dir
    src_dir = os.path.dirname(os.path.abspath(wavs[0]))
    r = subprocess.run([SIM, out, ""] + [os.path.abspath(w) for w in wavs],
                       cwd=src_dir, capture_output=True, text=True)
    if not os.path.exists(out):
        print(f"  {name}: 合成失敗\n{r.stdout[-800:]}")
        return None

    # Range rule: the timbre bank's span plus one octave up (same as the firmware and the sim)
    start, count = midis[0], (midis[-1] + 12) - midis[0] + 1
    js = os.path.join(workdir, f"{name}.json")
    e = subprocess.run([sys.executable, EVAL, out, src_dir,
                        "--start", str(start), "--count", str(count),
                        "--json", js], capture_output=True, text=True)
    if not os.path.exists(js):
        print(f"  {name}: 評測失敗\n{e.stdout[-800:]}\n{e.stderr[-400:]}")
        return None
    with open(js, encoding="utf-8") as f:
        d = json.load(f)
    if verbose:
        print(e.stdout)
    return {"mean": d["mean"], "n": len(d["notes"]),
            "range": f"{start}-{midis[-1]}", "notes": d["notes"]}


def collect(lib):
    """Return {instrument name: [wav paths...]}, single notes only."""
    out = {}
    for d in sorted(os.listdir(lib)):
        p = os.path.join(lib, d)
        if not os.path.isdir(p):
            continue
        wavs = [w for w in sorted(glob.glob(os.path.join(p, "*.wav")) +
                                  glob.glob(os.path.join(p, "*.WAV")))
                if midi_of(w) is not None]
        # Fewer than 3 notes is no "timbre bank", and not worth putting in the benchmark
        if len(wavs) >= 3:
            out[d] = wavs
    return out


def fmt(k, v):
    return "—" if v is None else CRITERIA[k][1](v)


def better(k, new, old):
    """+1 if new beats old, -1 if worse, 0 if identical."""
    if new is None or old is None:
        return 0
    a, b = (abs(new), abs(old)) if k in ABS_METRICS else (new, old)
    if abs(a - b) < EPS:
        return 0
    if k in HIGHER_BETTER:
        return 1 if a > b else -1
    return 1 if a < b else -1


def main():
    ap = argparse.ArgumentParser(description="多樂器合成品質回歸基準")
    ap.add_argument("--lib", required=True, help="聲音庫根目錄（底下每個子資料夾一種樂器）")
    ap.add_argument("--save", default=None, help="把這次結果存成基準 JSON")
    ap.add_argument("--compare", default=None, help="跟先前的基準比對")
    ap.add_argument("--only", default=None, help="只跑某一種樂器")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    groups = collect(args.lib)
    if args.only:
        groups = {k: v for k, v in groups.items() if k == args.only}
    if not groups:
        print("找不到任何有 3 個以上單音素材的資料夾", file=sys.stderr)
        sys.exit(1)

    work = tempfile.mkdtemp(prefix="tc_bench_")
    results = {}
    print(f"工作目錄 {work}\n")
    for name, wavs in groups.items():
        print(f"跑 {name}（{len(wavs)} 個素材）…")
        r = run_one(name, wavs, work, args.verbose)
        if r:
            results[name] = r

    if not results:
        print("沒有任何樂器跑成功", file=sys.stderr)
        sys.exit(1)

    # ------------------------------------------------------- output table --
    hdr = f"{'樂器':<10}{'音數':>4}  " + "".join(f"{CRITERIA[k][0][:6]:>10}" for k in METRIC_KEYS)
    print("\n" + hdr)
    print("-" * len(hdr))
    for name, r in results.items():
        row = f"{name:<10}{r['n']:>4}  " + "".join(f"{fmt(k, r['mean'][k]):>10}" for k in METRIC_KEYS)
        print(row)

    if args.save:
        with open(args.save, "w", encoding="utf-8") as f:
            json.dump({n: {"mean": r["mean"], "n": r["n"]} for n, r in results.items()},
                      f, ensure_ascii=False, indent=2)
        print(f"\n已存基準：{args.save}")

    # ----------------------------------------------------------- compare ---
    if args.compare:
        with open(args.compare, encoding="utf-8") as f:
            base = json.load(f)
        print("\n跟基準比對（+ 變好，- 變差，空白 = 沒動）")
        print("-" * len(hdr))
        regressions = []
        for name, r in results.items():
            if name not in base:
                print(f"{name:<10}  （基準裡沒有這一項，跳過）")
                continue
            cells = []
            for k in METRIC_KEYS:
                new, old = r["mean"][k], base[name]["mean"].get(k)
                d = better(k, new, old)
                if d == 0:
                    cells.append(f"{'':>10}")
                else:
                    delta = new - old
                    cells.append(f"{('+' if d > 0 else '-')}{abs(delta):>9.3f}")
                    if d < 0:
                        regressions.append((name, k, old, new))
            print(f"{name:<10}{'':>4}  " + "".join(cells))

        print()
        if regressions:
            print(f"** 有 {len(regressions)} 項退步 **")
            for name, k, old, new in regressions:
                print(f"   {name:<8} {CRITERIA[k][0]:<12} {fmt(k, old)} -> {fmt(k, new)}")
        else:
            print("沒有任何指標退步。")

    print(f"\n（合成檔與逐音 JSON 都留在 {work}，要細看隨時進去）")


if __name__ == "__main__":
    main()

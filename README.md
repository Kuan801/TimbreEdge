# TimbreClone

A Teensy 4.1 synthesizer that learns the timbre of one recorded note and plays that instrument back at any pitch. Input is a WAV on the SD card, or two seconds recorded live through the Audio Shield microphone.

1. Analyze -- `analyzer.cpp` extracts a timbre fingerprint from a single sustained note: YIN fundamental (median of 9 candidates), 32 harmonic amplitude tracks from a 2048-point FFT taken every 512 samples, per-harmonic onset delay and shimmer depth by heterodyne demodulation at 2.9 ms resolution, a 64-point log spectral envelope, ADSR with sustain decay rate, aperiodic noise ratio, and inharmonicity coefficient B. The result is an `InstrumentProfile`, saved to `PROFILE.BIN`.
2. Learn the timbre -- `timbre_model.cpp` maps `[log2(f0/261.63), loudness, log-time position, released]` to 32 harmonic weights (softmax) plus one noise gain (sigmoid) through a 4-32-32-33 MLP: 2305 parameters, 9 KB, one inference every 2.9 ms. Harmonics 33-64 are extrapolated from the measured spectral envelope. Without `MODEL.BIN` it falls back to 32 keyframes on the same log time axis. This is the DDSP idea -- the network predicts synthesizer parameters, not waveforms.
3. Synthesize -- `additive_synth.cpp` runs 8 voices of up to 64 harmonics each, with per-harmonic asynchronous onset, per-harmonic shimmer, an attack noise burst, and a band-limited noise layer. Spectral-envelope correction keeps formants in place under transposition; `TC_PARTIAL_BUDGET` (320) scales harmonic counts down as voices stack up.
4. Play -- `score.cpp` and `player.cpp` drive a chromatic scale (`PLAY.WAV`, for measurement) and a three-voice canon (`CANON.WAV`, for listening). A plain USB computer keyboard plays notes through the USB Host port (`kbd_in.cpp`); a four-button OLED menu (`ui.cpp`) covers the same operations without a computer.

Nothing is ever sampled back. Every note in the output comes from the additive synthesizer; the source recording is only used to extract the fingerprint.

## Hardware

| Part | Notes |
|---|---|
| Teensy 4.1 | 600 MHz Cortex-M7, 1 MB RAM |
| Teensy Audio Shield Rev D | SGTL5000 codec, 44.1 kHz stereo I2S |
| microSD 32 GB | FAT32 or exFAT. Prefer the built-in slot on the back of the Teensy (SDIO, much faster than the shield's SPI); the firmware falls back to the shield slot |
| Microphone | Electret capsule on the shield's MIC pads (board bias), or LINE IN |
| OLED 128x64 | I2C, SDA = 18, SCL = 19, 3.3 V. Address 0x3C, codec 0x0A. Needs the U8g2 library. Set `TC_USE_OLED` to 0 to omit it |
| Headphones / speaker | Shield HEADPHONE or LINE OUT |

`Audio`, `SD`, `SPI` and `Wire` ship with Teensyduino; U8g2 must be installed separately. Set Tools -> USB Type to `Serial`, serial monitor at 115200.

## Quick start

1. Open `TimbreClone.ino` in the Arduino IDE, select Teensy 4.1, upload.
2. Open the serial monitor at 115200.
3. Type `r` and play one sustained note into the microphone for about two seconds.
4. Analysis runs automatically and the OLED reports whether the take is usable (peak, SNR, onset count, decay) before it plays the result back.

To use an existing file instead, copy a 16-bit WAV to the SD root and type `a MYNOTE.WAV`.

## Serial commands

| Command | Action |
|---|---|
| `r` | Record from the microphone to `REC.WAV`, then analyze and play |
| `s` | Continuous sampling -- detect, record, name by pitch and add to the bank, unattended |
| `a` / `a FILE.WAV` | Analyze a WAV on the SD card (default `REC.WAV`) |
| `n [target]` | Analyze and add to the training set. Target: blank, a filename, several filenames, `*` (all WAVs in the root), or `SAMPLES/` |
| `t` / `t 8000` | Train the MLP on the Teensy, then save `MODEL.BIN`, apply it, play and record |
| `z` | Clear the training set |
| `p` / `x` | Play the chromatic scale / stop |
| `w` | Play and record to `PLAY.WAV` |
| `j` / `j n` | Play the canon and record to `CANON.WAV` / play without recording |
| `v` | Show the pitch coverage of the timbre bank |
| `k` | A/B the MLP blend weight between 0 and `TC_MLP_BLEND_AB` (0.35) |
| `l` / `m` | Reload `PROFILE.BIN` / `MODEL.BIN` |
| `g [0-63]` | Microphone gain |
| `o` | Input level monitor -- prints codec registers, then live RMS and peak |
| `i` | Switch between microphone and line input |
| `d` | List the SD root, retrying the mount if needed |
| `c` | Toggle `FRAMES.CSV` export for desktop training |
| `u` | Toggle the USB MTP disk (requires USB Type `Serial + MTP Disk`) |
| `y y` / `y s` / `y s SET02` | Delete generated WAVs / all sampling folders / one folder |
| `+` `-` / `?` | Master volume / help |

Commands are case-insensitive.

## Timbre bank

One profile per instrument means the whole range is reached by transposition, and two octaves out it stops sounding like the instrument. Each analyzed file is stored in a bank instead (up to 16 profiles, about 73 KB, saved as `BANK.BIN`), and every note picks the profile closest in pitch, which keeps transposition within a semitone. When the bank is full, the profile whose removal least increases the sum of squared spacings is evicted; range endpoints are never evicted.

Measured on 12 Iowa MIS piano notes, C4-B4:

| Metric | Single profile | 12-profile bank |
|---|---|---|
| Envelope correlation r | 0.973 | 0.992 |
| Harmonic LSD, sustain | 5.5 dB | 1.5 dB |
| Spectrogram MAE | 3.4 dB | 1.4 dB |
| Centroid correlation r | 0.174 | 0.895 |

For notes the bank cannot cover, the source is resampled at the target harmonic frequencies rather than scaled by a static envelope ratio, so each harmonic inherits the time trajectory of the frequency it actually lands on. The resampling weight follows transposition distance -- unused within one semitone, full at four or more (`TC_TRANSPOSE_RESAMPLE_LO` / `_HI`). On trumpet with a C4-B4 bank, the second octave improves from 2.30 to 1.79 dB LSD at onset and 0.705 to 0.771 centroid r, while the first octave is bit-for-bit unchanged.

## Training on the device

```
n *              load every WAV in the SD root
n SET01/         load one sampling folder
n A3.WAV E4.WAV  load specific files
t                train, then save MODEL.BIN, apply it, play and record
```

Files are loaded in filename order so results are reproducible. Progress is printed live:

```
[TRAIN] training set: 1274 samples, 3 pitches, 104 KB
[TRAIN] start: 1274 samples, batch 128, 6000 epochs, lr 0.0030
        epoch  6000   CE 2.0351   mean harmonic error 0.00023   (22 s)
```

Training takes 20 to 40 seconds (about 5.0 G MAC in total) and uses 210 KB for the training set plus 27 KB of Adam state. The audio ISR keeps running throughout. The training set lives in RAM and is lost at power-off; `MODEL.BIN` is on the SD card and is reloaded at boot. On-device and desktop training share the same features, loss, Adam hyperparameters and even the same Pade approximation of `tanh`, and both converge to a mean harmonic error near 0.0002.

To train on a desktop instead (numpy only, hand-written backpropagation):

```bash
python3 tools/train_ddsp.py violin_a3.wav violin_e4.wav violin_c5.wav -o MODEL.BIN
python3 tools/train_ddsp.py --csv FRAMES.CSV -o MODEL.BIN
```

Copy the resulting `MODEL.BIN` (9224 bytes) to the SD root; the Teensy loads it at boot or on `m`.

## Desktop simulator

`tools/sim` renders the same chromatic scale on a desktop using the same analyzer, timbre model, synthesizer, score and player sources as the firmware.

```bash
cd tools/sim
make
./sim ../../note.wav MODEL.BIN out.wav   # MODEL.BIN may be ""
./sim train MODEL.BIN a3.wav e4.wav c5.wav
```

The range rule matches the firmware (bank coverage plus one octave). The only difference from hardware is the absence of the SD, DAC and analog path.

## Objective evaluation

Build the bank from clean sources, let the machine play the chromatic scale and record it, compare note by note against the originals, and generate a Word report.

```bash
cd tools
python3 evaluate.py PLAY.WAV /path/to/sources/ \
        --start 60 --count 24 --json report.json --plot report.png
python3 report_docx.py report.json --plot report.png --out report.docx
```

`--start` must match the synthesized range (bank coverage plus one octave, not a fixed C3); a wrong value misaligns every pair silently. Press `v` or read the `[SCORE]` line to confirm. Requires numpy, matplotlib and python-docx.

Nine metrics are reported: envelope correlation r (good above 0.95), decay time error (under 200 cents), LSD at onset / sustain / release (under 3 dB), spectrogram MAE (under 3 dB, noise floor 1.3), centroid correlation r (above 0.9), noise level error (within 3 dB) and noise placement (under 8 points). The last two exist because the noise layer sits around -70 dB, where LSD, spectrogram and centroid cannot see it -- but the ear can. The report states measurements and criteria only; it draws no conclusions. `tools/resid_pos.py` adds absolute per-band residual levels, which the percentage-based placement metric cannot show.

`tools/bench.py` runs the whole set across instruments before and after a change, so an improvement in one instrument cannot hide a regression in another. Current numbers:

| Instrument | Notes | Env r | LSD onset | LSD sustain | LSD release | Spectrogram | Centroid r | Noise level | Noise placement |
|---|---|---|---|---|---|---|---|---|---|
| Flute | 12 | 0.890 | 1.0 | 0.7 | 0.8 | 0.8 | 0.863 | -2.3 | 7.5 |
| Guitar | 5 | 0.940 | 0.9 | 0.6 | 0.6 | 0.4 | 0.982 | +0.7 | 7.6 |
| Piano | 12 | 0.968 | 1.5 | 1.3 | 2.5 | 0.1 | 0.955 | -1.8 | 12.8 |
| Trumpet | 32 | 0.097 | 2.2 | 1.1 | 1.3 | 1.1 | 0.805 | -4.0 | 5.9 |
| Violin | 12 | 0.094 | 1.1 | 0.9 | 1.8 | 1.6 | 0.735 | -2.3 | 13.0 |

Envelope correlation is reported as unmeasurable when the reference envelope is flatter than `ENV_FLAT_STD_DB` (3 dB), which is the normal case for sustained tones; the low violin and trumpet figures are that, plus a score whose 1.818 s note length does not match the source recordings.

## One code path, no instrument branches

Nothing is switched on an instrument label. Class labels turned out to be unreliable -- 12 `Violin.arco` samples had no vibrato at all, verified two independent ways -- and a user recording an unclassified instrument would have no branch to take. Every difference is driven by a measured continuous quantity instead. The fraction of residual energy above 5*f0 is 2% for piano, 20% for flute and 89% for violin, which separates them without any label; below that frequency the residual is reproduced as per-harmonic amplitude jitter, above it as a band-passed noise layer whose upper edge is the frequency where the instrument's own spectral envelope falls 35 dB below its peak (2.3-3.4 kHz for piano, 7.0 kHz for trumpet).

## Recording

The analyzer needs one clean, sustained, single pitch.

- Hold the note for the full two seconds without a bow or breath change.
- Aim for a peak of 0.3 to 0.8. `micGain` is 0-63.
- Keep the room quiet; background noise is measured as part of the noise ratio.
- No chords -- YIN gives up.
- Pitch between 65 Hz and 1500 Hz (`TC_F0_MIN` / `TC_F0_MAX`).

Every take ends with a verdict panel -- peak, SNR, onset count, decay -- and a failed take stops on screen and is discarded rather than played back. Thresholds come from measured bad takes and have desktop tests (`make reccheck_test`, `make recscan`).

Do not play a software instrument through a speaker and record it with the microphone. Measured against clean piano sources, that path loses 27 dB at 250-500 Hz and adds 3 to 7 dB at 1.5-3.2 kHz; the resulting recording is spectrally closer to a violin than to the piano it came from (`tools/sim/envdist`: 3.91 and 5.69 dB from the clean piano, against 4.90 dB between clean piano and violin). In order of preference: export a WAV to the SD card, use a 3.5 mm cable into LINE IN (press `i`), or, failing both, at least avoid built-in speakers and set the gain properly. This cannot be detected automatically -- real trumpets have the same weak-fundamental signature, so any threshold that catches a bad take also catches every low trumpet note.

## Resource usage

| Item | Usage |
|---|---|
| Flash | ~130 KB |
| RAM (DTCM) | ~65 KB (synthesizer 15 KB, timbre model with weights 9 KB) |
| RAM (OCRAM) | ~330 KB (FFT and onset analysis 78 KB, training set 210 KB, Adam 27 KB) |
| CPU (4 voices plus tails) | ~40%, capped by `TC_PARTIAL_BUDGET` |
| SD | 5 s recording 441 KB; `PROFILE.BIN` 4664 bytes; `MODEL.BIN` 9224 bytes; `PLAY.WAV` ~8 MB |

Analysis blocks for 1 to 3 seconds inside `loop()`; the audio ISR is unaffected.

## Project layout

```
TimbreClone.ino        Main loop, audio graph, serial commands
config.h               Every tunable parameter
analyzer.h/.cpp        FFT / YIN / harmonic extraction / ADSR / spectral envelope
profile.h/.cpp         Timbre fingerprint structure and storage
timbre_model.h/.cpp    MLP inference, keyframe fallback, transposition correction
trainer.h/.cpp         On-device training: training set plus Adam backpropagation
additive_synth.h/.cpp  8-voice additive synthesizer (custom AudioStream)
score.h/.cpp           Chromatic scale and canon, both following the bank's range
player.h/.cpp          Tick scheduler
recorder.h/.cpp        Microphone streaming to SD
wav_io.h/.cpp          16-bit PCM WAV read/write and SD initialization
rec_check.h/.cpp       Take verdict rules (pure numeric, desktop-tested)
trigger.h/.cpp         Continuous-sampling trigger (pure state machine, desktop-tested)
kbd_in.h/.cpp          USB computer keyboard as keys (mapping, chords, transpose)
keys.h/.cpp            12 physical key buttons
buttons.h/.cpp         4 menu buttons (debounce, hold-to-repeat)
ui.h/.cpp              OLED menu state machine (pure logic, no Arduino calls)
display.h/.cpp         128x64 OLED status panel (U8g2)
demo/                  Pre-rendered demonstration audio
tools/train_ddsp.py    Offline training, outputs MODEL.BIN (numpy only)
tools/evaluate.py      Objective evaluation: envelope, LSD, centroid, aperiodics
tools/bench.py         Multi-instrument regression baseline
tools/report_docx.py   Turns evaluation output into a Word report
tools/resid_pos.py     Absolute per-band residual levels
tools/sim/             Desktop simulator, same DSP sources as the firmware
```

`tools/sim` also builds the test suite: `kbd_test`, `ui_test`, `score_test`, `reccheck_test`, `trigger_test`, `setdir_test`, `evict_test`, `wavhdr_test`, plus `usbcheck` and `inocheck`, which compile the USB path and the `.ino` against stub headers before flashing.

## Limitations

- Additive synthesis reconstructs harmonic structure. Percussion, distorted guitar and vocal consonants will not sound right; strings, winds, piano and guitar will.
- One timbre at a time. Changing instrument means recording again.
- Beyond two octaves of transposition, spectral-envelope correction starts to break down. Training the MLP on more pitches helps.
- Polyphony over USB is limited by the keyboard: the HID boot protocol reports 6 keys, and membrane keyboards without anti-ghosting may drop at 3. The synthesizer has 8 voices.
- No velocity. Buttons and keyboards cannot measure it and the source material is single-velocity (`Trumpet.vib.ff`), so louder-is-brighter cannot be reproduced. It would take separate pp / mf / ff profiles per note.
- USB MIDI was removed, not fixed. The device enumerated, the descriptors and the bulk IN endpoint were correct, and not one message ever arrived; the same keyboard works on a computer. The old code is in `舊版備份/midi_in.cpp`.
- `PROFILE.BIN` and `MODEL.BIN` formats changed; older files are ignored by size and need re-analysis and retraining.

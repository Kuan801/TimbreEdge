# TimbreEdge

A Teensy 4.1 synthesizer that learns the timbre of a recorded instrument and lets you play it back as a full, pitch-shifted playable keyboard.

1. Analyze -- `analyzer.h/.cpp` extracts a timbre fingerprint from one sustained note: fundamental, harmonic amplitude tracks, per-harmonic onset delay and shimmer, spectral envelope, ADSR, noise ratio, and inharmonicity.
2. Learn the timbre -- a DDSP-style MLP (`timbre_model.h/.cpp`, `trainer.h/.cpp`) predicts harmonic weights and noise gain from pitch, loudness and time, driving a physical synthesizer instead of emitting waveforms. It trains on the Teensy itself, or on a desktop through `tools/train_ddsp.py`.
3. Render playable notes -- `additive_synth.h/.cpp` is a polyphonic additive synthesizer with per-harmonic onset, shimmer and a band-limited noise layer; spectral-envelope correction keeps formants in place under transposition, and a timbre bank holds one fingerprint per sampled pitch so each note plays from the closest one.
4. Play -- a plain USB computer keyboard plays notes through the Teensy's USB Host port (`kbd_in.h/.cpp`), a four-button OLED menu (`ui.h/.cpp`) covers the same operations without a computer, and `score.h/.cpp` plays a chromatic scale for measurement and a three-voice canon for listening.

Hardware

Teensy 4.1 with a Teensy Audio Shield Rev D, a microSD card, an electret microphone on the shield's MIC pads (or LINE IN), and a 128x64 I2C OLED. Optionally a USB A socket wired to the USB Host header.

Setup

Install the U8g2 library from the Arduino Library Manager, set Tools -> USB Type to `Serial`, open `TimbreClone.ino`, select Teensy 4.1 and upload. Serial monitor at 115200.

Running

```
r      record a note from the microphone, analyze it, play it back
n *    analyze every WAV on the card into the timbre bank
t      train the model on the device
p      play the chromatic scale
?      list every command
```

Or work through the OLED menu, with no computer attached.

Project Layout

```
TimbreClone.ino        Main loop, audio graph, serial commands
config.h               Every tunable parameter
analyzer.h/.cpp        FFT / YIN / harmonic extraction / ADSR / spectral envelope
profile.h/.cpp         Timbre fingerprint structure and storage
timbre_model.h/.cpp    MLP inference, keyframe fallback, transposition correction
trainer.h/.cpp         On-device training
additive_synth.h/.cpp  Polyphonic additive synthesizer (custom AudioStream)
score.h/.cpp           Chromatic scale and canon, both following the bank's range
player.h/.cpp          Tick scheduler
recorder.h/.cpp        Microphone streaming to SD
wav_io.h/.cpp          16-bit PCM WAV read/write and SD initialization
rec_check.h/.cpp       Take verdict rules (pure numeric, desktop-tested)
trigger.h/.cpp         Continuous-sampling trigger (pure state machine)
kbd_in.h/.cpp          USB computer keyboard as keys (mapping, chords, transpose)
keys.h/.cpp            Physical key buttons
buttons.h/.cpp         Menu buttons (debounce, hold-to-repeat)
ui.h/.cpp              OLED menu state machine (pure logic, no Arduino calls)
display.h/.cpp         OLED status panel (U8g2)
demo/                  Pre-rendered demonstration audio
tools/train_ddsp.py    Offline training, outputs MODEL.BIN (numpy only)
tools/evaluate.py      Objective evaluation: envelope, LSD, centroid, aperiodics
tools/bench.py         Multi-instrument regression baseline
tools/report_docx.py   Turns evaluation output into a Word report
tools/sim/             Desktop simulator and test suite
```

Evaluation

`tools/evaluate.py` compares a recorded chromatic scale against the original samples note by note, and `tools/report_docx.py` turns the result into a Word report that states measurements and criteria but draws no conclusions. `tools/bench.py` runs the same comparison across every instrument before and after a change, so an improvement in one cannot hide a regression in another. `tools/sim/` renders the same scale on a desktop from the same DSP sources as the firmware, and builds the test suite.

Team

TimbreEdge is one half of a two-part capstone project built by a four-person team. Chia-Hui Yang and I-Pei Chen were primarily responsible for TimbreEdge (this repository); Xuan-Yun Weng and Wen-Ting Chen were primarily responsible for the project's other half, Timbre Shadowing, a desktop app that learns an instrument's timbre and plays it back as a playable keyboard. All four members contributed to system integration, testing, and the project report.

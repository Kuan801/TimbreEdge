// ============================================================================
//  config.h  -  TimbreClone for Teensy 4.1 + Audio Shield (SGTL5000)
//  Global parameters. Change them here, don't scatter them across files.
// ============================================================================
#pragma once

#include <Arduino.h>

// ------------------------------------------------------------ Audio basics --
#define TC_SAMPLE_RATE      44100.0f
#define TC_BLOCK            AUDIO_BLOCK_SAMPLES      // 128
#define TC_BLOCK_SEC        (TC_BLOCK / TC_SAMPLE_RATE)   // 2.902 ms

// ----------------------------------------------------- Analysis parameters --
#define TC_FFT_SIZE         2048        // 46.4 ms window
#define TC_HOP              512         // 11.6 ms hop

// Two separate notions of harmonic count:
//   TC_N_HARM     how many are analyzed and modeled one by one. Past 32, a real
//                 recording is mostly buried in noise, so modeling each one
//                 individually is really learning the noise.
//   TC_N_PARTIAL  the cap on how many are actually synthesized. Amplitudes for
//                 33~64 are extrapolated from the spectral envelope, which is
//                 more robust than per-harmonic modeling and also stops the bass
//                 sounding muffled: D2(73Hz) used to top out at 1184 Hz, now it
//                 reaches 4.7 kHz.
#define TC_N_HARM           32
#define TC_N_PARTIAL        64

#define TC_MAX_FRAMES       512         // Max RMS envelope slots (~6 s)
#define TC_N_KEYFRAME       32          // Keyframes for timbre over time

// Fine attack analysis: window and range of the heterodyne demodulation
#define TC_ATK_WINDOW_SEC   0.30f       // How long to look after the attack
#define TC_ATK_HOP          128         // 2.9 ms resolution, enough to resolve asynchrony of tens of ms
#define TC_ATK_LP_HZ        120.0f      // Low-pass after demodulation, sets how smooth the envelope is
#define TC_SPECENV_PTS      64          // Spectral envelope sample points (log frequency)
#define TC_SPECENV_FMIN     50.0f
#define TC_SPECENV_FMAX     16000.0f

// YIN f0 search range
#define TC_F0_MIN           65.0f       // C2
#define TC_F0_MAX           1500.0f     // ~F#6
#define TC_YIN_THRESH       0.15f

// ---------------------------------------------------- Synthesis parameters --
#define TC_N_VOICES         8           // Polyphony (4-voice canon + headroom for release tails)
// USB Host computer keyboard input. On for the Teensy, off for the desktop
// simulator because it has no USBHost_t36.
//
// This used to be TC_USE_USB_MIDI. The USB MIDI keyboard never received a single
// packet, start to finish (enumeration fine, descriptors correct, just no data);
// the cause was never found, so the whole thing was removed in favour of a plain
// USB computer keyboard. The old code is in 舊版備份/midi_in.cpp.
#ifndef TC_USE_USB_KBD
  #if defined(__IMXRT1062__)
    #define TC_USE_USB_KBD 1
  #else
    #define TC_USE_USB_KBD 0
  #endif
#endif

#define TC_SHIMMER_HZ_MIN   2.5f        // Frequency range over which each harmonic wanders independently
#define TC_SHIMMER_HZ_MAX   7.5f

// Compensation for harmonic jitter (energy multiplier).
//
// additive_synth states its goal plainly: the aperiodic ratio measured on the
// synthesized signal by period differencing has to equal the noiseGain the
// analyzer measured. In practice it always came up short -- after the noise
// sampling window was fixed, the noise-level error of all four instruments
// landed neatly in -2.0 ~ -3.3 dB (flute -3.1, piano -2.9, trumpet -3.3,
// violin -2.0). Four instruments, four registers, all in the same direction:
// that is a missing coefficient in the compensation chain, not four coincidences.
//
// There are two acknowledged approximations in that chain: the closed form for
// the mean sideband gain, and the measured value 0.83 for "the extra attenuation
// caused by interpolation". Both are first-order only; this constant makes up
// the rest. It applies to the jitter layer only (the broadband noise layer is
// uncorrelated noise, period differencing loses nothing on it, so it needs no
// compensation).
#ifndef TC_JITTER_CAL
#define TC_JITTER_CAL       1.9f

#endif
// How to decide "how loud harmonic h of the target should be" when transposing.
//
//   0 = old method: take the source's harmonic h and multiply it by the spectral
//       envelope's gain ratio between the old and the new frequency
//   1 = resampling: ask directly "how loud is the source at this absolute
//       frequency", i.e. resample the source's harmonic series at the target's
//       harmonic frequencies (interpolated in the log domain)
//
// For the "average timbre" the two are almost the same (formants stay put either
// way); the difference is in **time**: the old method hands the **time variation**
// of the source's harmonic h straight to the target's harmonic h, but transposing
// up an octave puts the target's harmonic h at the frequency of the source's
// harmonic 2h -- what it should follow is 2h's movement, not h's. Measured on
// trumpet (bank C4~B4, played up to B5, with real material in the second octave
// to compare against):
//
//                        LSD attack   LSD sustain   centroid r
//   octave 1 (interp)       1.14         0.66          0.924
//   octave 2 (extrap)       2.30         1.53          0.705    <- old method
//
// The mean centroid is right (synth/reference = 0.99); what is wrong is its
// variation over time -- exactly what "the wrong time trajectory was copied"
// would produce. When f0Play equals the reference pitch the two are identical,
// so notes the timbre bank covers do not change by a single bit.
#define TC_TRANSPOSE_RESAMPLE   1        // 0 = use the old method entirely (the code is not even compiled in)
// Blend weight: 1 = all resampling, 0 = all envelope gain ratio.
// Two macros because #if cannot take a float as its condition.
#define TC_TRANSPOSE_RESAMPLE_W 1.0f
// Weighted by transposition distance in semitones: none within LO, full above HI.
#define TC_TRANSPOSE_RESAMPLE_LO 1.0f
#define TC_TRANSPOSE_RESAMPLE_HI 4.0f

// Global partial budget: normal playing never gets near it, but stacked release
// tails (worst case 8 voices x 64 = 512 partials) would blow the CPU up. Over
// budget, every voice's harmonic count is scaled back proportionally -- better
// slightly duller than clipping. Measured: 320 partials is about 40% CPU.
#define TC_PARTIAL_BUDGET   320
#define TC_SINE_TBL_BITS    10
#define TC_SINE_TBL_SIZE    (1 << TC_SINE_TBL_BITS)     // 1024
#define TC_NYQUIST_GUARD    0.45f       // Harmonics above 0.45*fs are muted outright

// Whether the harmonic peak search window widens with "how far this harmonic
// could have moved".
//
// A fixed +-2 bins (+-43 Hz with the 46 ms window) is enough for instruments
// with no inharmonicity, but harmonic 12 of piano C4 sits 67 Hz sharp when
// B = 0.0003 -- outside the window, so the search can only stop at the window
// edge and the measured shift is truncated. The old B estimator happened to read
// high as well (see the notes at analyzer.cpp 4b), the two errors cancelled, and
// so the piano "looked" like it was measured accurately. Once the estimator was
// fixed, the truncation could no longer hide: the piano's inharmonicity was
// judged to be 0 across the board.
//
// Turning it off (=0) sends the piano's noise position back from 12.8 to
// 14.5 pt; the other instruments are unaffected.
#ifndef TC_PEAK_WIDE
#define TC_PEAK_WIDE        1
#endif

// Start of the shimmer sampling window: how long after the attack to begin.
// Too early swallows the attack transient; too late brings back the old
// "not enough frames, just call it 0" problem.
// Measured: 0.15 / 0.30 / 0.45 all change the violin estimate by less than 0.01,
// so take the shortest.
#ifndef TC_SHIM_START_SEC
#define TC_SHIM_START_SEC   0.15f
#endif

// Cap on shimmer.
//
// It used to be 0.20. After the sampling window was widened (see shimA in
// analyzer.cpp) the high register is no longer forced to 0, but the two short,
// high samples Ab5/B5 measure 0.20~0.26 and pile up against the cap right across
// the board. Measured, that is not one or two outlier harmonics (the median is
// just as high), the whole distribution is high -- those two recordings are
// simply unsteady. An estimate stuck at the cap pours excessive amplitude
// modulation into the synthesis side.
//
// Sweep (violin; the other instruments are completely unaffected):
//   cap      centroid r  noise pos
//   0.20       0.650        10.8   <- worse than not changing anything
//   0.15       0.669        10.8
//   0.12       0.700        10.7   <- take this one, better than the original 0.690
//
// 0.12 is also closer than 0.20 to profile.h's own "real instruments are around
// 5~10%".
// To be honest about it: this number was swept on the four instruments we have,
// that is the whole sample, and another set of material may well prefer a
// different value -- it is a cap, not a measured physical quantity.
#ifndef TC_SHIM_MAX
#define TC_SHIM_MAX         0.12f
#endif

// Cap on the harmonic jitter depth sigma.
//
// Jitter is applied like this (additive_synth 2b):
//     target[h] *= clamp(1 + sigma * jit[h], 0, 2.5)   jit ~ U(-sqrt3, +sqrt3)
// For the multiplier to stay non-negative you need sigma <= 1/sqrt(3) = 0.5774.
// Past that both ends hit the wall, and the jitter stops being "a small wobble
// around 1" and becomes a random switch re-thrown every 2.9 ms -- which is
// exactly that gritty hiss at the onset of a note.
//
// Measured on the user's BANK.BIN (sigma over the first 150 ms of the attack,
// and the fraction that hits the wall):
//     B5  sigma 2.27 -> 37% forced to zero, 31% pinned at 2.5x, 68% in total
//     C5  sigma 1.32 -> 45%      A#4 sigma 1.23 -> 41%
//     F4  sigma 0.73 -> 10%      D#4 sigma 0.68 ->  8%
// 8 of the 16 have sigma > 0.6, which matches "a lot of notes have a gritty
// attack" precisely.
//
// And the derivation itself assumes a small signal: sideband energy =
// 2*sigma^2*mean(sin^2) simply does not hold once it clips, so the energy that
// actually goes out is neither equal to the measured value nor free of a pile of
// broadband distortion.
//
// The variance above the cap must not just be thrown away -- measured, the
// synthesized attack's aperiodic ratio is only 0.2x the reference, which is
// already too little. So it overflows into the broadband noise layer instead
// (see the notes in additive_synth).
#ifndef TC_JIT_SIGMA_MAX
#define TC_JIT_SIGMA_MAX    0.5774f
#endif

// Order of the noise layer's low-pass. 1 = 6 dB/oct (old), 2 = 12 dB/oct.
//
// Why it has to be steeper: a first-order skirt is too gentle to hold the bow
// noise in. Measured on the violin's aperiodic component, the material has 62%
// in 2~5 kHz and only 6.7% above 5 kHz; the first-order version had just 35% in
// 2~5 kHz and shot up to 29% above 5 kHz -- the bow noise itself missing, a layer
// of hiss added. The cause is that a first-order low-pass rolls off only
// 6 dB/oct above the corner (at A4 the corner is 3.5 kHz, so it is 6 dB down only
// at 7 kHz and 12 dB down at 14 kHz), so white noise fed into it naturally leaves
// a mass of high frequency behind.
//
// Measured sweep (the "noise position" metric of the four instruments, in
// percentage points, smaller is better; < 8 is good, < 15 acceptable):
//
//   order   flute   piano   trumpet    violin   note
//     1      8.5     12.9     9.8       18.5    old version
//     2      7.7     11.9     8.9       16.0
//     3      7.6     11.5     8.5       15.1    <- take this: the knee of the curve
//     4      7.6     11.3     8.4       14.6
//     6       -       -        -        14.0    beyond this it turns back (order 8: 14.4)
//
// All four instruments improve monotonically, not one gets worse; the violin's
// spectrogram MAE also drops from 1.7 to 1.5. Returns fall off fast past order 3,
// and the ~14 points left over are not a slope problem (no amount of steepness
// brings them down), they are the band-pass centre and shape still not fitting
// closely enough -- that needs a separate change, not more orders.
// Cost: 2 flops per order per sample, about 0.7 MFLOP/s with 8 voices, negligible.
// How many biquads to chain on the low-pass end of the noise band-pass
// (2 poles = 12 dB/oct each).
//
// The old version chained three first-order recursions, and that structure has
// one fatal degeneracy: y += c*(x - y) with c close to 1 is just y = x. And
// c = 1 - exp(-2*pi*fc/fs), so pushing each stage's corner up to 15 kHz in the
// high register gives c = 0.89 -- the so-called third-order low-pass was not
// filtering at all. A biquad uses the bilinear transform, so however high the
// corner it is still a low-pass and the problem cannot arise.
//
// 3 stages (= 6 poles, 36 dB/oct) is where the measurements land: at 2 stages the
// piano's attack is still 6 dB above the real material above 5 kHz, at 3 stages
// it drops to -5 dB (slightly cleaner than the real material, on the safe side).
// Cost: 5 multiply-adds per stage per sample, about 1.8 MFLOP/s with 8 voices.
#ifndef TC_NOISE_LP_STAGES
#define TC_NOISE_LP_STAGES  3
#endif

// Where the top edge of the noise layer goes: how many dB below the spectral
// envelope's peak counts as "this instrument ends here".
//
// This is the real cause of the gritty attack. The old top edge was 8*f0, with no
// reference at all to "does this instrument still have energy in that band".
// The noise layer of piano F#5 was therefore placed at 3.7~5.9 kHz, while the
// piano's own spectral envelope is already -40 dB at 2.5 kHz and -67 dB at 5 kHz.
// Measured in situ: above 5 kHz during the attack, the noise layer is 15~23 dB
// louder than the piano's own harmonics. That layer lands in a band where the
// instrument is completely empty, nothing masks it, and that is the grit.
//
// specEnv is the spectral envelope already measured at analysis time (log
// frequency 50 Hz~16 kHz, in dB), and existing BANK.BIN files already carry it --
// using it as the ceiling needs no re-analysis.
// Taking the frequency 35 dB below the peak, the four instruments land at:
//    piano 2.3~3.4 kHz   guitar 3.7 kHz   flute 4.9 kHz
//    violin 6.4 kHz      trumpet 7.0 kHz
// That ordering agrees with the independent "90th percentile of residual energy"
// measurement (piano 1.8 kHz, flute 3.3 kHz, violin 3.7 kHz), which says it
// really is describing the same thing.
//
// Why the threshold is 35 and not 30 or 40 -- absolute level error (dB) of the
// >5 kHz residual during the attack:
//    thresh   piano   violin  flute   trumpet   mean
//    old       15.2    1.6     3.6      15.1    8.9
//     30       6.4     1.5     14.3     14.6    9.2   flute's breath noise cut by 14 dB
//     35       5.1     2.3     5.7      15.4    7.1 <- all four within 6 dB
//     40       2.6     3.3     3.3      16.8    6.5   violin starts to overshoot
// The trumpet's 15 dB is not caused by this layer (turning the broadband layer
// off changes nothing), it is a problem with the attack's jitter layer.
#ifndef TC_NOISE_ROLL_DB
#define TC_NOISE_ROLL_DB    35.0f
#endif

// Volume multiplier of the broadband noise layer. 1.0 = exactly the measured
// noiseGain.
//
// This layer's "amount" is measured and its level is right, so normally it
// should not be touched -- the reason this knob exists is debugging: setting it
// to 0 still runs the loop and still consumes the random numbers, it just does
// not add the result to the output, so subtracting the gain=0 output from the
// gain=1 output gives exactly this layer with nothing else disturbed.
// The difference method breaks for any ablation that changes random-number
// consumption (been there, see the failure log).
#ifndef TC_NOISE_BB_GAIN
#define TC_NOISE_BB_GAIN    1.0f
#endif

// ---------------------------------------------------------- MLP parameters --
// DDSP-lite:  [pitch, loudness, time, released] -> 32 harmonic weights + 1 noise gain
#define TC_MLP_IN           4
#define TC_MLP_H1           32
#define TC_MLP_H2           32
#define TC_MLP_OUT          (TC_N_HARM + 1)
// Scale of the MLP pitch feature: in[0] = log2(f0/261.63) / TC_MLP_PITCH_SCALE
// It was 3.0 (meant to cover +-4.5 octaves), but real training material often
// spans just one octave, so the whole data set only used an input range of
// 0~0.3, the tanh network could not separate adjacent semitones, and it just
// emitted "the average distribution over all pitches" -- measured, h2 was
// averaged away by 11 percentage points and h4~h7 grew energy a real piano does
// not have. Changing it to 1.0 lets one octave fill 0~0.92.
#define TC_MLP_PITCH_SCALE  1.0f

// Weight of the MLP in the final timbre: 0 = use the measured keyframes only,
// 1 = use the MLP only.
//
// Log-spectral distance measured on real piano material (Iowa MIS, C4~B4,
// 12 semitones):
//     BLEND 0.0 -> 1.8 dB      0.35 -> 2.6 dB
//     BLEND 0.7 -> 3.8 dB      1.0  -> 5.0 dB
// The keyframes are measured directly and are the most faithful; what the MLP
// learns smooths detail away and also leaks energy into harmonics the real
// instrument simply does not have (measured, about 1% extra on each of h6/h7).
//
// Hence the default 0: if the material has only one dynamic level (all mf, say),
// the MLP has nothing to learn and can only blur an accurate measurement.
// When to raise it: once you have pp / mf / ff of the same notes, the MLP finally
// earns its keep (the nonlinear dynamics-to-timbre relation), and then 0.3~0.5
// is the better choice.
#ifndef TC_MLP_BLEND
#define TC_MLP_BLEND        0.0f
#endif

// The other weight that serial 'k' switches to for A/B. 0.35 is the point in the
// table above where "the MLP starts to intervene audibly but has not yet wrecked
// the timbre" -- just right for demonstrating the difference live.
//
// The old 'k' toggled "are the weights loaded" (_hasMlp), but with the default
// BLEND=0 both sides sounded identical, making it a switch with no effect. What
// should be switched is the blend weight.
#ifndef TC_MLP_BLEND_AB
#define TC_MLP_BLEND_AB     0.35f
#endif

#define TC_MLP_MAGIC        0x324D4C50u   // 'PLM2' (feature scale changed, old models unusable)
#define TC_PROFILE_MAGIC     0x31504355u   // Added the vibratoHz field; bumped so an old BANK.BIN is rejected, not misread   // 'TCP1'

// Total flattened parameter count (w1,b1,w2,b2,w3,b3), needed by Adam
#define TC_MLP_NPARAM  (TC_MLP_H1 * TC_MLP_IN + TC_MLP_H1 +                  \
                        TC_MLP_H2 * TC_MLP_H1 + TC_MLP_H2 +                  \
                        TC_MLP_OUT * TC_MLP_H2 + TC_MLP_OUT)   // = 2305
// Breakdown: weights 32x4 + 32x32 + 33x32 = 2208, biases 32 + 32 + 33 = 97, 2305 total (9.0 KB)
//
// This comment once said 1777 -- a leftover from the days when TC_N_HARM was
// still 16 (a 17-dim output), never updated after the move to 32 harmonics.
// The macro itself was always right, only the comment was wrong, but the slides
// and the docs both copied that number. Either compute a number from the macro,
// or change it along with the macro.

// ---------------------------------------------------- On-device training ----
// Training data goes in DMAMEM(OCRAM). 21 floats per record = 84 bytes.
//   Once the harmonic count became 32, keeping the targets as float would have
//   swollen this to 148 bytes/record. Stored instead as "square-root companding
//   + int16": small amplitudes actually get better relative resolution than the
//   low bits of a plain float32, each record stays at 84 bytes, and 2560 records
//   is about 215 KB.
//   The Teensy 4.1 has 512 KB of OCRAM, and with the analyzer and the weights the
//   total is about 320 KB, so there is room to spare.
#define TC_TRAIN_MAX        2560
#define TC_TRAIN_BATCH      128
#define TC_TRAIN_EPOCHS     6000
#define TC_TRAIN_LR         0.003f
#define TC_TRAIN_NOISE_W    0.3f       // Weight of the noise term in the loss

// ---------------------------------------------------- Recording parameters --
// Recording length. Measured (tools/evaluate.py, one run each over 12 notes of
// piano and of violin):
//
//    length     env r   LSD sus    spec MAE   centroid r   verdict
//     5.0 s     0.993     1.5        1.4        0.968      baseline
//     3.0 s     0.993     1.5        1.4        0.967      no difference at all
//     2.0 s     0.993     1.5        1.4        0.975      no difference at all
//     1.5 s     0.990     1.5        1.5        0.978      almost no difference
//     1.0 s     0.891     1.5        2.3        0.965      envelope starts to drift
//     0.7 s     0.513     1.5        4.5        0.757      clearly degraded
//     0.5 s     0.303     1.5        5.1        0.539      unusable
//
// Key observation: the harmonic distribution (LSD) is still 1.5 dB even at 0.5 s
// -- the timbre is settled a few hundred milliseconds after the attack. What
// really needs time is the envelope, because you have to see the decay trend.
// So 2 seconds is enough. The upside: REC.WAV drops from 441 KB to 176 KB, the
// recording wait is 3 seconds shorter, and most importantly "hold it steady for
// a full 2 seconds" is far easier than "hold it steady for a full 5 seconds" --
// so the material quality actually improves.
// (Analysis time is dominated by fixed costs, so shorter material only speeds it
// up so much: measured, less than 2x.)
#define TC_REC_SECONDS      2
#define TC_REC_PATH         "REC.WAV"

// ------------------------------------------------- Mic continuous sampling --
// After pressing S you never touch the keyboard again: it detects sound, records
// a take, analyzes it, names the file by pitch, saves it, adds it to the timbre
// bank, and returns to waiting. One person with an instrument can sample the
// whole range.
#define TC_TRIG_LEVEL       0.035f   // Trigger threshold (relative to full scale). Raise it in a noisy room

// --- Detecting "swapped the instrument but forgot to clear" -----------------
//
// On insertion, compute how far the new timbre is from the closest entry already
// in the bank, and warn if that exceeds the threshold. A warning only, nothing
// is blocked.
//
// These two numbers are measured, not guessed. Material: trumpet 33 notes
// (E3~B5), piano/violin/flute 12 notes each (C4~B4), 68 in all, 2278 pairings.
// Tool: tools/sim/envdist.
//
//   Threshold 2.0, with >= 3 entries in the bank:
//     same-instrument false-alarm rate       1.07%
//     piano <-> wind/string detection rate    100%
//     wind vs. string detection rate          ~67%
//
//   With only 1 entry in the bank the false-alarm rate jumps to 9.64%, so it
//   stays quiet until there are at least 3.
//
// The honest limitation: only 4 instruments, and the weights are hand-set. It
// catches obvious cases like "wind swapped for piano"; trumpet swapped for
// trombone it probably cannot tell apart. Treat it as a hint, not a verdict.
#define TC_TIMBRE_WARN_DIST     2.0f
#define TC_TIMBRE_WARN_MIN_REFS 3
// How many consecutive blocks have to exceed the threshold to count as a real
// attack. 2 blocks is only 5.8 ms, which a noise spike manages easily; 4 blocks
// is 11.6 ms, and real instrument attacks are all far longer than that (measured,
// the fastest is the piano, climbing to half its peak in 5.8 ms), so no real note
// is missed.
#define TC_TRIG_BLOCKS      4

// --- Ambient adaptation of the trigger threshold ----------------------------
//
// TC_TRIG_LEVEL is now a floor, not a threshold. Actual threshold =
// max(floor, ambient noise x margin).
//
// Why this had to change (measured on the 12 files the user actually recorded,
// taking the quietest 0.3 s of each, 1236 blocks in all):
//
//     background block peak   median 0.0207   99% 0.0281   max 0.0301
//     weakest note            median block peak 0.0529
//     the old fixed threshold 0.035
//
// The background maximum is only 1.16x (1.3 dB) below the threshold. One more fan
// in the room and it records all night long -- which is exactly what happened.
//
// Margin of 1.5: background max 0.0301 x 1.5 = 0.045, which keeps the noise out,
// while the weakest note at 0.0529 still gets through. But that leaves only
// 1.4 dB of headroom, which is very thin -- so TriggerGate works out the actual
// headroom and reports it to the user, and says so when it is too thin to use.
#define TC_TRIG_MARGIN      1.5f
#define TC_TRIG_CAL_MS      600      // How long to listen after sampling starts, to measure the ambient noise
// What counts as "quiet".
//
// The first version said "below 0.6x the threshold", and desktop testing caught
// the problem straight away: the threshold is itself ambient noise x 1.5, so
// 0.6 x threshold = 0.9 x ambient noise -- the ambient noise on its own keeps
// crossing that line (measured crest factor 1.46), so "400 ms of continuous
// quiet" is never reached, it never arms, and not a single note is captured.
// Fixing the spurious triggering by never triggering at all is just as useless.
//
// The correct meaning is "back down to the level of the ambient noise", so it has
// to be referenced to the ambient noise and not to the threshold:
//     quiet line   = max(ambient noise x 1.20, configured floor x 0.60)
//     trigger line = max(ambient noise x 1.50, configured floor)
// The gap between the two lines is hysteresis: the signal has to be clearly above
// ambient to trigger, and has to fall back to ambient before it re-arms.
#define TC_TRIG_QUIET_MARGIN 1.20f
#define TC_TRIG_QUIET_FRAC   0.60f

// How many dB above the ambient noise the signal has to be to be usable. Below
// this value, whether a trigger really caught a note becomes a matter of luck.
// 12 dB = 4x, a reasonable floor for "you can tell which is which".
#define TC_TRIG_MIN_HEADROOM_DB  12.0f
#define TC_PREROLL_BLOCKS   32       // 93 ms of pre-roll: by the time it triggers the attack has already happened,
                                     // without this buffer the most critical attack transient gets cut off
#define TC_REARM_SILENT_MS  400      // How long it has to stay quiet after a take before re-arming
#define TC_PROFILE_PATH     "PROFILE.BIN"
#define TC_MODEL_PATH       "MODEL.BIN"
#define TC_PLAY_PATH        "PLAY.WAV"     // The take recorded while playing the chromatic scale
#define TC_CANON_PATH       "CANON.WAV"    // The take recorded while playing the canon
                                           // Kept as two files: the chromatic scale
                                           // is for evaluation, the canon for
                                           // listening; overwriting one means
                                           // recording it again.
                                           // CANON.WAV is PLAY.WAV's old name, which
                                           // wav_io's "generated by the program" rule
                                           // already recognizes, so y y still deletes it
#define TC_BANK_PATH        "BANK.BIN"     // Multi-pitch timbre bank

// Max WAVs to scan on a batch load
#define TC_MAX_SCAN_FILES   32
#define TC_MAX_NAME_LEN     32

// ---------------------------------------------------- USB MTP (disk drive) --
//  Mounts the SD card as a drive on the computer, so the finished WAVs and
//  BANK.BIN can be fetched without pulling the card out. For remote maintenance
//  this is the only way to get files back to a computer.
//
//  Two things are needed to use it:
//    1. Arduino IDE -> Tools -> USB Type -> "Serial + MTP Disk (Experimental)"
//    2. install the MTP_Teensy library (not bundled with Teensyduino)
//         https://github.com/KurtE/MTP_Teensy
//
//  With the wrong USB Type, TC_HAS_MTP is 0 and the whole section compiles out --
//  so the same source still builds under the ordinary "Serial" USB Type and does
//  not fall apart just because one library is missing.
//  (MTP_Teensy.h has its own #error, so the include must stay inside the
//  conditional.)
#if defined(USB_MTPDISK) || defined(USB_MTPDISK_SERIAL)
#define TC_HAS_MTP          1
#else
#define TC_HAS_MTP          0
#endif

//  Whether MTP is on by default after boot.
//
//  Why this switch is needed: PJRC's own Teensyduino 1.57 announcement says
//  there is still a lot of work to do before MTP and the Teensy can access files
//  at the same time, and MTP_Teensy issue #41 records SD writes timing out and
//  failing while MTP is mounted, and that "the built-in SD socket is worse than
//  the audio shield's SPI socket" -- this project's tcSdBegin() prefers exactly
//  that built-in SDIO socket, landing squarely on the worst combination. And the
//  writes here are not optional: during playback this is a live 44.1 kHz stereo
//  stream being written to file, and one dropped block is one click in the
//  finished take.
//
//  So there are three lines of defence: the default switch (here), loop() only
//  servicing MTP when completely idle, and the serial u command to turn it off
//  on the spot.
#ifndef TC_MTP_DEFAULT_ON
#define TC_MTP_DEFAULT_ON   1
#endif

// -------------------------------------------------------------- OLED panel --
// Set to 0 with no panel attached; nothing else is affected (the desktop
// simulator builds with -DTC_USE_OLED=0)
#ifndef TC_USE_OLED
#define TC_USE_OLED         1
#endif

// Driver IC. Three are common on 128x64 modules; they look almost identical but
// their memory maps differ:
//   SSD1306 : 0.96", the most common
//   SH1106  : 1.3", the one you have if the image is shifted 2 pixels sideways
//   SSD1309 : 1.54" / 2.42"
// Just set one of these three lines to 1 and the other two to 0.
#define TC_OLED_SSD1306     1
#define TC_OLED_SH1106      0
#define TC_OLED_SSD1309     0

#define TC_OLED_I2C_HZ      400000     // Drop to 100000 if the display is noisy
#define TC_OLED_REFRESH_MS  200        // A full 128x64 frame @400kHz takes about 20 ms, so don't refresh too often

// -------------------------------------------------------------------- Pins --
// Prefer the Teensy 4.1's built-in SD (SDIO), fall back to the Audio Shield's
// SPI SD only if that fails
#define TC_SDCARD_CS_PIN    10
#define TC_SDCARD_MOSI_PIN  11
#define TC_SDCARD_SCK_PIN   13

// Optional physical buttons (active low, INPUT_PULLUP). They can be left out;
// everything is reachable through serial commands too.
// Four tactile switches: one leg to the pins below, the other to GND (internal
// pull-ups, no external resistors needed).
//
// 2~5 were chosen because they are four consecutive free holes on the audio
// shield's bottom row, right next to the GND in the bottom-right corner -- all
// four buttons can share that one GND, with the shortest wiring.
//
// The pins the audio shield Rev D actually uses (from the official table):
//     audio data 7 8 20 21 23 / control 18 19 / SD card 10 11 12 13 / memory chip 6
//     volume pot 15 (free if no pot is soldered on)
// So the holes left free on the shield are 0 1 2 3 4 5 9 14 15 16 17 22, twelve
// in all.
// (An early version of this comment said the shield uses 9, 15 and 22; that was
// wrong.)
#define TC_BTN_UP           2
#define TC_BTN_DOWN         3
#define TC_BTN_OK           4
#define TC_BTN_BACK         5

// Debounce: a state has to hold this long to be accepted. Tactile switches mostly
// bounce for 1~5 ms, longer with a poor contact; 20 ms is loose enough without
// feeling sluggish.
#define TC_BTN_DEBOUNCE_MS      20
// Auto-repeat once the up/down keys are held (so micGain 0~63 doesn't take 63 presses)
#define TC_BTN_REPEAT_DELAY_MS  400
#define TC_BTN_REPEAT_MS        120

// 12 physical keys, C4~B4. One leg of each to a pin, the other to GND (all 12
// share one GND wire).
//
// White keys on the near row, black keys on the far row -- matching the layout of
// a real keyboard, and each group stays contiguous:
//
//     white keys (row with 0~12)   C4=24  D4=25  E4=26  F4=27  G4=28  A4=29  B4=30
//     black keys (row with 13~23)  C#4=34 D#4=35 F#4=36 G#4=37 A#4=38
//
// The tail of the Teensy 4.1 is already two separate rows (near 24~33, far
// 34~41), so rather than forcing 24~35 and leaving 10 pins in one row and 2 in
// the other, split them the way a keyboard already is. When wiring, "the top row
// is all black keys, the bottom row all white", so a wire in the wrong place is
// obvious at a glance.
//
// Left unused: 31 32 33 (near) and 39 40 41 (far).
//
// This area is out of the audio shield's reach, so there is no conflict with the
// shield at all.
// Wiring all 12 directly instead of a 4x3 matrix: a matrix ghosts as soon as
// three keys are held, and preventing that means a diode in series with every
// key; and this synth has 8 voices, so direct wiring is what actually lets you
// play chords.
//
// The order is always C C# D D# E F F# G G# A A# B; changing pins means changing
// only this one line.
#define TC_KEY_PINS { 24, 34, 25, 35, 26, 27, 36, 28, 37, 29, 38, 30 }

// ------------------------------------------------------------- Performance --
#define TC_BPM              66.0f
#define TC_TICKS_PER_BEAT   4           // Sixteenth-note resolution
#define TC_MAX_NOTES        224

// --------------------------------------------------------------- Utilities --
static inline float tc_midiToHz(float midi) {
  return 440.0f * powf(2.0f, (midi - 69.0f) / 12.0f);
}
static inline int tc_clampi(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static inline float tc_clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Padé approximation of tanh, error < 0.3%, but an order of magnitude faster
// than tanhf() (the M7 has no hardware transcendentals, tanhf costs 50~100
// cycles).
// Inference and training MUST use the same one, otherwise you get a
// train/inference mismatch.
static inline float tc_tanh(float x) {
  if (x < -3.0f) return -1.0f;
  if (x >  3.0f) return  1.0f;
  float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
static inline float tc_sigmoid(float x) {
  return 0.5f * (1.0f + tc_tanh(0.5f * x));
}

// --------------------------------------------------------------- Time warp --
// The attack lasts only tens of milliseconds, yet it has to share one set of
// keyframes with a sustain that runs for seconds -- allocate them linearly and
// each frame is 150 ms, so the whole attack does not even fill one frame, which
// means it is not modeled at all.
// On a logarithmic time axis the first 200 ms gets about 1/3 of the frames and
// the first 500 ms gets half.
//
// ★ The analyzer, inference, the MLP's 3rd input feature and the Python training
//   script must all use the same definition, or training and inference will not
//   line up.
#define TC_TIME_WARP_TAU    0.06f
static inline float tc_timeWarp(float tSec, float noteDur) {
  if (noteDur < 1e-3f) noteDur = 1e-3f;
  if (tSec < 0.0f) tSec = 0.0f;
  const float k = 1.0f / TC_TIME_WARP_TAU;
  return logf(1.0f + k * tSec) / logf(1.0f + k * noteDur);
}

// How many harmonics this pitch should emit: pack them in up to Nyquist, but no
// more than the cap. Low notes get the full 64 (D2 -> 4.7 kHz), high notes
// naturally get fewer (A5 at 880Hz needs only 22).
static inline int tc_partialCount(float f0) {
  if (f0 <= 1.0f) return 1;
  int n = (int)(TC_SAMPLE_RATE * TC_NYQUIST_GUARD / f0);
  if (n < 1) n = 1;
  if (n > TC_N_PARTIAL) n = TC_N_PARTIAL;
  return n;
}

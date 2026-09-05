// ============================================================================
//  profile.h  -  the data structure for one "instrument timbre fingerprint"
//  Produced by the analyzer, consumed by the model, and storable as PROFILE.BIN
//  on the SD card
// ============================================================================
#pragma once

#include <Arduino.h>
#include "config.h"

struct InstrumentProfile {
  uint32_t magic;            // TC_PROFILE_MAGIC

  float f0;                  // Reference fundamental as analyzed (Hz)
  float noteDur;             // Effective note length (seconds, from onset to tail)

  // --- amplitude envelope (fitted ADSR values, used during playback) ---
  float attack;              // seconds
  float decay;               // seconds
  float sustain;             // 0..1
  float release;             // seconds
  // Natural decay factor per second during sustain: 1.0 = no decay at all (organ /
  // sustained strings), 0.5 = halves every second (piano / plucked). Measured only
  // inside the sustain window, release excluded.
  float sustainDecayPerSec;

  // --- timbre ---
  // keyframe[k][h] = relative amplitude of harmonic h at time point k
  // (normalized, sums to 1)
  float keyframe[TC_N_KEYFRAME][TC_N_HARM];
  // loud[k] = overall loudness at time point k (0..1)
  //
  // ★ This curve is the amplitude envelope actually used during synthesis; the
  //   parametric ADSR is no longer used.
  //   Why: a piano has a two-stage decay (prompt sound + aftersound). Measured,
  //   the first 0.3 s decays at 0.026/s and after 0.8 s only 0.70/s remains -- a
  //   factor of 27. Fitting a single exponential gives 0.475/s, which for the
  //   0.45~1.8 s notes in the canon is almost 20 times too slow, so every note
  //   drags on like an organ. Playing back the measured curve directly avoids
  //   that, and it also gets the violin's crescendo and the wind attack right for
  //   free.
  float loud[TC_N_KEYFRAME];

  // Where (on the warped time axis) the envelope should stop falling.
  //   decaying   -> 1.0, run the whole curve; the decay is the instrument's sound
  //   sustaining -> the end of the body, held flat after that, so that "lifting the
  //                 bow" is not performed as an automatic fade-out
  float envHoldNorm;

  // --- spectral envelope (formants), TC_SPECENV_PTS points evenly spaced on a log
  //     frequency axis from 50 Hz to 16 kHz, in dB ---
  // Used for formant preservation under transposition: change the pitch, not the
  // timbre.
  float specEnv[TC_SPECENV_PTS];

  float noiseGain;           // Aperiodic (breath / bow noise) energy fraction during sustain, 0..1
  float inharmonicity;       // Coefficient B, > 0 for stringed instruments; fine-tunes harmonic frequencies
  float brightness;          // Spectral centroid / f0, for debugging

  // --- the three realism ingredients (all measured from the source, not guessed) ---

  // Asynchronous onset: the moment each harmonic reaches 50% of its own peak,
  // relative to the fundamental (seconds). On real instruments the upper harmonics
  // usually arrive tens of milliseconds late; starting them all together sounds
  // extremely electronic.
  float harmOnset[TC_N_HARM];

  // Per-harmonic micro-fluctuation depth during sustain (std/mean, with the overall
  // decay removed). Real instruments are around 5~10%; with none at all, sustained
  // notes sound like an organ.
  float shimmerDepth;

  // Noise fraction in the 30 ms before the onset. Bow noise, breath noise and
  // string-strike noise concentrate here, usually far above the sustain noiseGain.
  float attackNoise;

  // Vibrato depth measured from the source (cents). Violin / voice / winds have it;
  // piano and guitar are essentially 0. Early versions added a flat 6 cents of
  // vibrato to everything -- a vibrating piano is physically impossible, and that
  // uniform wobble gave every instrument the same "synthesizer" tint.
  float vibratoCents;
  float vibratoHz;           // Measured vibrato rate (0 when there is no vibrato)
  float noiseHighFrac;       // Fraction of the sustain residual above 5*f0 (where breath noise vs bow noise sits)
  float attackHighFrac;      // Fraction of the attack residual above 5*f0 (where hammer noise vs breath noise sits)

  bool  valid;
};

// ============================================================================
//  ProfileBank  -  several sample points for one instrument
//
//  With only one profile, the entire range has to be carried by "transposition +
//  spectral envelope correction". A real piano's timbre differs a lot between
//  registers (multiple strings in the bass, a single string up top, a different
//  strike-point ratio), so two octaves of transposition will never sound right.
//
//  With 12 source recordings, 12 profiles are stored and each note uses the
//  closest one, shrinking the transposition distance to a semitone and taking the
//  error close to zero. 16 profiles occupy about 75 KB of DMAMEM.
// ============================================================================
#define TC_MAX_PROFILES 16

struct ProfileBank {
  InstrumentProfile p[TC_MAX_PROFILES];
  // Whether the last add() suspected "this may not be the same instrument".
  // The panel has to show it -- while sampling, the user is holding an instrument
  // and watching the OLED, not the serial monitor.
  bool  lastAddSuspect = false;
  float lastAddDist    = 0.0f;
  int n = 0;

  void clear() { n = 0; lastAddSuspect = false; lastAddDist = 0.0f; }
  bool add(const InstrumentProfile &np);        // The same pitch overwrites
  void checkTimbreMismatch(const InstrumentProfile &np);   // Called internally by add()
  int  nearest(float f0) const;                 // Find the closest one; returns -1 when empty

  // Who to sacrifice when the bank is full. Returns the index to be replaced, or
  // -1 meaning "the new one is not worth the swap".
  //
  // Public purely for testing: this is pure selection logic (it looks only at
  // pitch, never at audio), so it can be verified completely on a desktop -- and
  // when it goes wrong it fails silently, simply leaving some register with no
  // source material.
  int  evictionTarget(float newF0) const;
  const InstrumentProfile *get(float f0) const;
  void summary() const;
};

bool  bankSave(const ProfileBank &b, const char *path);
bool  bankLoad(ProfileBank &b, const char *path);

bool  profileSave(const InstrumentProfile &p, const char *path);
bool  profileLoad(InstrumentProfile &p, const char *path);
void  profilePrint(const InstrumentProfile &p);

// Interpolate the spectral envelope on the log frequency axis; returns linear gain
// (not dB)
float specEnvGain(const InstrumentProfile &p, float hz);

// How far apart two timbres' spectral envelopes are (dB, RMS).
//
// The spectral envelope is deliberately designed to be pitch-independent (it
// describes the resonating body, not the note being blown or plucked), so
// different pitches of the same instrument should be close together and only a
// different instrument should pull them apart.
// Use: detect "the instrument was changed but the bank was not cleared first".
//
// Only the shape is compared, not the absolute level: each side has its own mean
// subtracted before the difference. A different recording level should not count
// as a different timbre.
float profileEnvDistance(const InstrumentProfile &a, const InstrumentProfile &b);

// Combined timbre distance (dimensionless, 1.0 ≈ the typical difference between
// two pitches of the same instrument).
//
// The spectral envelope alone is not enough: measured, the envelope distance
// *within* one piano reaches 15 dB, further apart than "trumpet vs violin" --
// because every piano note has different strings and hammers. But the piano is
// very far from sustaining instruments on decay rate, inharmonicity and shimmer,
// and only taking all of them together separates "the instrument changed" from
// "a different note on the same instrument".
float profileTimbreDistance(const InstrumentProfile &a, const InstrumentProfile &b);

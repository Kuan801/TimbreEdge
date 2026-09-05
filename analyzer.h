// ============================================================================
//  analyzer.h  -  offline (non-real-time) analysis of a single-note WAV,
//                 producing an InstrumentProfile
//
//  Steps:
//    1. Scan the whole file once for the RMS envelope -> onset / offset / ADSR
//    2. Run YIN on the steadiest frames to get f0 (take the median)
//    3. Per-frame FFT, parabolic interpolation around h*f0 for harmonic amplitudes
//    4. Compress every frame down to TC_N_KEYFRAME keyframes
//    5. Extract the log spectral envelope (formants) from the loudest frame
//    6. Estimate the aperiodic noise ratio as "total energy - harmonic energy"
//
//  This runs blocking inside loop(), roughly 2~6 s for a 5 s source. The audio
//  ISR keeps running throughout, so there is no dropout.
// ============================================================================
#pragma once

#include <Arduino.h>
#include "config.h"
#include "profile.h"

class TrainSet;      // trainer.h

// Analysis blocks for 1~3 s, so there is a progress callback to stop the OLED
// looking like a hang. frac is 0..1. Pass nullptr to cancel.
void analyzerSetProgressCallback(void (*cb)(float frac));

// How many onsets the last analysis counted in the recording. 1 = a normal
// single note.
//
// >= 2 means the take contains more than one pluck or stroke, and the whole
// profile is untrustworthy: ADSR, decay rate, shimmer and noise ratio all rest
// on the single-note assumption. Measured on a guitar take with 6 plucks, the
// decay went from a true 0.5 to 0.95 (the synthesis then sustains like an organ).
//
// The panel has to show this -- someone holding an instrument is not watching
// the serial monitor.
int analyzerLastOnsetCount();

// Recording quality measured by the last analysis. When the synthesis sounds
// wrong, the problem is usually in these three numbers rather than in the
// synthesizer:
//   Peak       absolute peak over the file. < 0.05 is too quiet (quantization
//              noise dominates); 1.0 means it already clipped (the waveform is
//              flattened and every harmonic is bogus).
//   ClipRatio  fraction of clipped samples. Above 0.001, turn micGain down.
//   NoiseFloor mean RMS before the onset (relative to peak). Above 0.1
//              (SNR < 20 dB) the trigger fired on room noise and the actual
//              note occupies only a small part of the recording.
float analyzerLastPeak();
float analyzerLastClipRatio();
float analyzerLastNoiseFloor();

// csvDumpPath : if non-NULL, dump every frame to CSV (for desktop training)
// trainSet    : if non-NULL, push every frame straight into the training set
//               (for on-device training). Both paths compute identical features
//               and targets, so the models they produce are equivalent.
bool analyzeWavFile(const char *wavPath,
                    InstrumentProfile &out,
                    const char *csvDumpPath = nullptr,
                    TrainSet *trainSet = nullptr);

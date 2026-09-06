#pragma once

#include <Arduino.h>
#include "config.h"
#include "profile.h"

class TrainSet;

void analyzerSetProgressCallback(void (*cb)(float frac));

int analyzerLastOnsetCount();

float analyzerLastPeak();
float analyzerLastClipRatio();
float analyzerLastNoiseFloor();

bool analyzeWavFile(const char *wavPath,
                    InstrumentProfile &out,
                    const char *csvDumpPath = nullptr,
                    TrainSet *trainSet = nullptr);

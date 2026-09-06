#pragma once

#include <Arduino.h>
#include "config.h"

struct InstrumentProfile {
  uint32_t magic;

  float f0;
  float noteDur;

  float attack;
  float decay;
  float sustain;
  float release;

  float sustainDecayPerSec;

  float keyframe[TC_N_KEYFRAME][TC_N_HARM];

  float loud[TC_N_KEYFRAME];

  float envHoldNorm;

  float specEnv[TC_SPECENV_PTS];

  float noiseGain;
  float inharmonicity;
  float brightness;

  float harmOnset[TC_N_HARM];

  float shimmerDepth;

  float attackNoise;

  float vibratoCents;
  float vibratoHz;
  float noiseHighFrac;
  float attackHighFrac;

  bool  valid;
};

#define TC_MAX_PROFILES 16

struct ProfileBank {
  InstrumentProfile p[TC_MAX_PROFILES];

  bool  lastAddSuspect = false;
  float lastAddDist    = 0.0f;
  int n = 0;

  void clear() { n = 0; lastAddSuspect = false; lastAddDist = 0.0f; }
  bool add(const InstrumentProfile &np);
  void checkTimbreMismatch(const InstrumentProfile &np);
  int  nearest(float f0) const;

  int  evictionTarget(float newF0) const;
  const InstrumentProfile *get(float f0) const;
  void summary() const;
};

bool  bankSave(const ProfileBank &b, const char *path);
bool  bankLoad(ProfileBank &b, const char *path);

bool  profileSave(const InstrumentProfile &p, const char *path);
bool  profileLoad(InstrumentProfile &p, const char *path);
void  profilePrint(const InstrumentProfile &p);

float specEnvGain(const InstrumentProfile &p, float hz);

float profileEnvDistance(const InstrumentProfile &a, const InstrumentProfile &b);

float profileTimbreDistance(const InstrumentProfile &a, const InstrumentProfile &b);

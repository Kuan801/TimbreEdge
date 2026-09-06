#pragma once

#include <Arduino.h>
#include "config.h"

enum TcState : uint8_t {
  TC_ST_BOOT = 0,
  TC_ST_IDLE,
  TC_ST_RECORDING,
  TC_ST_ANALYZING,
  TC_ST_TRAINING,
  TC_ST_PLAYING,
  TC_ST_ERROR
};

void displayBegin();
void displayScanI2C();

void displaySetState(TcState s, const char *detail = nullptr);

TcState displayState();

bool displayMenuVisible();
void displaySetProgress(float p);
void displaySetLine(int idx, const char *text);

void displaySetSystem(bool sdOk, bool hasModel, bool hasProfile);
void displaySetTrainInfo(int samples, int pitches);
void displaySetProfileInfo(float f0, const char *noteName);

void displaySetMenu(const char *title, const char (*rows)[26], int nRows,
                    int cursorRow, int firstRow, int totalRows, bool editing);

void displayService();
void displayForce();

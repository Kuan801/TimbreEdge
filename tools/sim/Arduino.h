// ============================================================================
//  tools/sim/Arduino.h  -  給桌機模擬器用的最小 Arduino 相容層
//  只是為了能在電腦上編譯 / 跑同一份 DSP 程式碼，不會燒進 Teensy。
// ============================================================================
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

#define DMAMEM
#define PROGMEM
#define F(x) (x)

extern uint64_t sim_micros;
static inline uint32_t micros() { return (uint32_t)sim_micros; }
static inline uint32_t millis() { return (uint32_t)(sim_micros / 1000ULL); }
static inline void     delay(uint32_t ms) { sim_micros += (uint64_t)ms * 1000ULL; }

#define INPUT_PULLUP 2
#define LOW  0
#define HIGH 1

// Teensy 的 Print.h 會定義這幾個巨集。模擬器一定要跟著定義，否則會出現
// 「桌機編得過、Teensy 編不過」的情況 —— 曾經有個區域變數叫 DEC，撞上
// Print.h 的 #define DEC 10，直到燒錄時才發現。
#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2
static inline void pinMode(int, int) {}
static inline int  digitalRead(int) { return HIGH; }

struct SimSerial {
  void begin(long) {}
  operator bool() const { return true; }
  int  available() { return 0; }
  int  read() { return -1; }
  template <typename... A> void printf(const char *f, A... a) { std::printf(f, a...); }
  void print(const char *s) { std::printf("%s", s); }
  void print(int v)         { std::printf("%d", v); }
  void print(float v)       { std::printf("%f", v); }
  void println()            { std::printf("\n"); }
  void println(const char *s) { std::printf("%s\n", s); }
};
extern SimSerial Serial;

// ============================================================================
//  tools/sim/Arduino.h  -  minimal Arduino compatibility layer for the desktop simulator
//  Only there so the same DSP code compiles / runs on a PC; never flashed to a Teensy.
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

// Teensy's Print.h defines these macros. The simulator must define them too, or you get
// "builds on the desktop, fails on the Teensy" -- there was once a local variable named
// DEC that collided with Print.h's #define DEC 10, and it only surfaced at flash time.
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

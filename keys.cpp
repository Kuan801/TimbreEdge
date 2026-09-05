#include "keys.h"

Keys gKeys;

static const uint8_t kKeyPins[TC_N_KEYS] = TC_KEY_PINS;
static const char   *kKeyNames[TC_N_KEYS] = {
  "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

void Keys::begin(AudioSynthAdditive *synth) {
  _synth = synth;
  for (int i = 0; i < TC_N_KEYS; i++) {
    _k[i].pin = kKeyPins[i];
    pinMode(_k[i].pin, INPUT_PULLUP);
  }
  Serial.print(F("[KEYS] 12 個琴鍵 C4~B4，腳位 "));
  for (int i = 0; i < TC_N_KEYS; i++) Serial.printf("%d%s", kKeyPins[i], i < TC_N_KEYS - 1 ? "," : "");
  Serial.println(F("（另一腳接 GND）"));
}

int Keys::downCount() const {
  int n = 0;
  for (uint16_t m = _mask; m; m >>= 1) if (m & 1) n++;
  return n;
}

void Keys::setTranspose(int8_t semi) {
  if (semi < -24) semi = -24;
  if (semi >  24) semi =  24;
  if (semi == _transpose) return;
  // Kill sounding notes before transposing, otherwise the release would noteOff
  // the new pitch and the old note would never stop
  if (_on) setEnabled(false), setEnabled(true);
  _transpose = semi;
}

void Keys::setEnabled(bool on) {
  if (on == _on) return;
  _on = on;
  if (!on && _synth) {
    for (int i = 0; i < TC_N_KEYS; i++) {
      if (_k[i].stable && _k[i].playing) _synth->noteOff((float)_k[i].playing);
      _k[i].stable  = false;
      _k[i].playing = 0;
    }
    _mask = 0;
  }
  Serial.printf("[KEYS] %s\n", on ? "琴鍵模式開啟" : "琴鍵模式關閉");
}

void Keys::service() {
  if (!_on || !_synth) return;
  const uint32_t now = millis();

  for (int i = 0; i < TC_N_KEYS; i++) {
    K &k = _k[i];
    const bool raw = (digitalRead(k.pin) == LOW);

    if (raw != k.raw) { k.raw = raw; k.changed = now; continue; }
    if ((now - k.changed) < TC_BTN_DEBOUNCE_MS) continue;

    if (raw && !k.stable) {
      k.stable = true;
      int midi = 60 + i + _transpose;          // 60 = C4
      if (midi < 0)   midi = 0;
      if (midi > 127) midi = 127;
      k.playing = (uint8_t)midi;
      _mask |= (uint16_t)(1u << i);
      // Fixed velocity -- a tactile switch cannot measure force. Velocity would
      // need pressure-sensing keys, and the source material is single-velocity
      // anyway, so it could not be reproduced even if we could measure it.
      _synth->noteOn((float)midi, 0.85f, 0.5f);

    } else if (!raw && k.stable) {
      k.stable = false;
      _mask &= (uint16_t)~(1u << i);
      // Release the pitch we actually sent, not a recomputed one -- a transpose
      // in between would make them disagree
      if (k.playing) _synth->noteOff((float)k.playing);
      k.playing = 0;
    }
  }
}

// For the panel: format the held keys as a string like "C E G"
void keysDownText(char *out, size_t cap) {
  out[0] = 0;
  size_t used = 0;
  const uint16_t m = gKeys.downMask();
  for (int i = 0; i < TC_N_KEYS && used + 4 < cap; i++) {
    if (!(m & (1u << i))) continue;
    int n = snprintf(out + used, cap - used, "%s%s", used ? " " : "", kKeyNames[i]);
    if (n > 0) used += (size_t)n;
  }
  if (!used) snprintf(out, cap, "---");
}

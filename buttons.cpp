#include "buttons.h"

Buttons gButtons;

void Buttons::begin() {
  const struct { uint8_t pin; UiKey key; bool rep; } cfg[4] = {
    { TC_BTN_UP,   UI_KEY_UP,   true  },
    { TC_BTN_DOWN, UI_KEY_DOWN, true  },
    { TC_BTN_OK,   UI_KEY_OK,   false },
    { TC_BTN_BACK, UI_KEY_BACK, false },
  };
  for (int i = 0; i < 4; i++) {
    _b[i].pin     = cfg[i].pin;
    _b[i].key     = cfg[i].key;
    _b[i].repeats = cfg[i].rep;
    _b[i].stable  = false;
    _b[i].raw     = false;
    _b[i].changed = 0;
    pinMode(_b[i].pin, INPUT_PULLUP);
  }
  Serial.printf("[BTN] 上=%d 下=%d 確定=%d 返回=%d（另一腳接 GND）\n",
                TC_BTN_UP, TC_BTN_DOWN, TC_BTN_OK, TC_BTN_BACK);
}

bool Buttons::anyDown() const {
  for (int i = 0; i < 4; i++) if (_b[i].stable) return true;
  return false;
}

UiKey Buttons::poll() {
  const uint32_t now = millis();

  for (int i = 0; i < 4; i++) {
    Btn &b = _b[i];
    const bool raw = (digitalRead(b.pin) == LOW);      // Pull-up, so pressed is LOW

    if (raw != b.raw) {                                // Raw state changed, restart the timer
      b.raw = raw;
      b.changed = now;
      continue;
    }
    if ((now - b.changed) < TC_BTN_DEBOUNCE_MS) continue;   // Not settled yet

    if (raw && !b.stable) {                            // Just pressed -> event
      b.stable  = true;
      b.downAt  = now;
      b.lastRep = now;
      return b.key;
    }
    if (!raw && b.stable) {                            // Released
      b.stable = false;
      continue;
    }
    // Held down: only up/down auto-repeat
    if (raw && b.stable && b.repeats &&
        (now - b.downAt) > TC_BTN_REPEAT_DELAY_MS &&
        (now - b.lastRep) > TC_BTN_REPEAT_MS) {
      b.lastRep = now;
      return b.key;
    }
  }
  return UI_KEY_NONE;
}

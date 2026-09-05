// ============================================================================
//  ui.h  -  4 buttons + OLED menu, replacing operation over the serial port
//
//  Wiring (one leg of each button to its Teensy pin, the other to GND; internal
//  pull-ups, no resistors needed)
//
//        Button      Pin     Meaning
//        UP           2      menu up; value +1 (auto-repeats when held)
//        DOWN         3      menu down; value -1 (auto-repeats when held)
//        OK           4      enter submenu / run / enter value editing
//        BACK         5      up one level / leave value editing / status screen back to menu
//
//  Pins 2 and 3 are the original record/play buttons, so those two need no rewiring,
//  only their meaning changed. 4 and 5 are new. On a Teensy 4.1 pins 4, 5, 14, 16,
//  17 and 24~33 are all free of the audio shield; to change a pin, change config.h.
//
//  --- Two design decisions ---------------------------------------------------
//
//  1) A menu item never does anything itself; it only returns a command string for
//     the .ino's handleCommand() to run. Recording, analysis and training therefore
//     have exactly one implementation each, and the serial port stays fully usable.
//     Otherwise the same feature would exist as two pieces of code, and one would
//     get changed while the other was forgotten.
//
//  2) This file touches nothing from Arduino (no digitalRead, no u8g2); the
//     navigation logic is a pure state machine. So it runs on the desktop and can
//     be tested -- "does pressing this get to the right place" should not depend on
//     flashing it and watching with your eyes.
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------- keys ------
enum UiKey : uint8_t {
  UI_KEY_NONE = 0,
  UI_KEY_UP,
  UI_KEY_DOWN,
  UI_KEY_OK,
  UI_KEY_BACK
};

// ----------------------------------------------------------- menu data ------
enum UiKind : uint8_t {
  UI_PAGE   = 0,   // Enter a subpage
  UI_CMD    = 1,   // Run a command
  UI_ADJUST = 2    // Value editing: once inside, up/down change the value, BACK leaves
};

struct UiItem {
  const char *label;
  UiKind      kind;
  uint8_t     target;      // UI_PAGE: subpage number
  const char *cmd;         // UI_CMD: command string; UI_ADJUST: a format containing one %d
  int16_t     vmin, vmax, vstep;
  int16_t    *value;       // UI_ADJUST: the actual variable (supplied by the .ino)
};

struct UiPage {
  const char   *title;
  const UiItem *items;
  uint8_t       n;
  uint8_t       parent;    // Which page BACK goes to; the root page points at itself
};

// ------------------------------------------------------------------ Ui -----
#define UI_VISIBLE_ROWS 4        // 128x64 fits 4 rows once the title and the underline are taken off
#define UI_MAX_DEPTH    4

class Ui {
public:
  void begin(const UiPage *pages, uint8_t nPages);

  // Feed in one key. Returns the command string to run, nullptr if there is none.
  // The pointer is into an internal buffer, so the caller must use it at once (handleCommand copies it).
  const char *feed(UiKey k);

  // --- for the drawing side -----------------------------------------------
  const char *title() const;
  uint8_t     rowCount() const;                 // How many items this page has
  uint8_t     cursor() const { return _cursor; }
  uint8_t     topRow() const { return _top; }
  // The text to display for item row (value items carry the current value)
  void        rowText(uint8_t row, char *out, size_t cap) const;
  bool        editing() const { return _editing; }

  // While a status screen is up (playing, analyzing, ...) the menu stands aside; wake it when that ends
  void        setSuspended(bool s) { _suspended = s; }
  bool        suspended() const { return _suspended; }

private:
  const UiPage *_pages = nullptr;
  uint8_t _nPages = 0;

  uint8_t _page   = 0;
  uint8_t _cursor = 0;
  uint8_t _top    = 0;
  bool    _editing   = false;
  bool    _suspended = false;

  // Stack for BACK: remembers the cursor position at each level, so going back lands on the same item
  uint8_t _stackPage[UI_MAX_DEPTH];
  uint8_t _stackCur[UI_MAX_DEPTH];
  uint8_t _depth = 0;

  char _cmdBuf[32];

  const UiItem *cur() const;
  void  clampScroll();
};

extern Ui gUi;

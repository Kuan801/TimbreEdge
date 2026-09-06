#pragma once

#include <stdint.h>
#include <stddef.h>

enum UiKey : uint8_t {
  UI_KEY_NONE = 0,
  UI_KEY_UP,
  UI_KEY_DOWN,
  UI_KEY_OK,
  UI_KEY_BACK
};

enum UiKind : uint8_t {
  UI_PAGE   = 0,
  UI_CMD    = 1,
  UI_ADJUST = 2
};

struct UiItem {
  const char *label;
  UiKind      kind;
  uint8_t     target;
  const char *cmd;
  int16_t     vmin, vmax, vstep;
  int16_t    *value;
};

struct UiPage {
  const char   *title;
  const UiItem *items;
  uint8_t       n;
  uint8_t       parent;
};

#define UI_VISIBLE_ROWS 4
#define UI_MAX_DEPTH    4

class Ui {
public:
  void begin(const UiPage *pages, uint8_t nPages);

  const char *feed(UiKey k);

  const char *title() const;
  uint8_t     rowCount() const;
  uint8_t     cursor() const { return _cursor; }
  uint8_t     topRow() const { return _top; }

  void        rowText(uint8_t row, char *out, size_t cap) const;
  bool        editing() const { return _editing; }

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

  uint8_t _stackPage[UI_MAX_DEPTH];
  uint8_t _stackCur[UI_MAX_DEPTH];
  uint8_t _depth = 0;

  char _cmdBuf[32];

  const UiItem *cur() const;
  void  clampScroll();
};

extern Ui gUi;

#include "ui.h"
#include <stdio.h>

Ui gUi;

// ---------------------------------------------------------------------------
void Ui::begin(const UiPage *pages, uint8_t nPages) {
  _pages = pages;
  _nPages = nPages;
  _page = 0;
  _cursor = 0;
  _top = 0;
  _depth = 0;
  _editing = false;
  _suspended = false;
  _cmdBuf[0] = 0;
}

const UiItem *Ui::cur() const {
  if (!_pages || _page >= _nPages) return nullptr;
  const UiPage &p = _pages[_page];
  if (_cursor >= p.n) return nullptr;
  return &p.items[_cursor];
}

const char *Ui::title() const {
  if (!_pages || _page >= _nPages) return "";
  return _pages[_page].title;
}

uint8_t Ui::rowCount() const {
  if (!_pages || _page >= _nPages) return 0;
  return _pages[_page].n;
}

// 讓游標永遠留在可視範圍內
void Ui::clampScroll() {
  if (_cursor < _top) _top = _cursor;
  if (_cursor >= _top + UI_VISIBLE_ROWS) _top = _cursor - (UI_VISIBLE_ROWS - 1);
  const uint8_t n = rowCount();
  if (n <= UI_VISIBLE_ROWS) _top = 0;
  else if (_top > n - UI_VISIBLE_ROWS) _top = n - UI_VISIBLE_ROWS;
}

void Ui::rowText(uint8_t row, char *out, size_t cap) const {
  out[0] = 0;
  if (!_pages || _page >= _nPages) return;
  const UiPage &p = _pages[_page];
  if (row >= p.n) return;
  const UiItem &it = p.items[row];

  if (it.kind == UI_ADJUST && it.value) {
    // 編輯中的項目用 [ ] 框起來，一眼看得出上下鍵現在在改什麼
    const bool ed = (_editing && row == _cursor);
    snprintf(out, cap, ed ? "%s [%d]" : "%s %d", it.label, (int)*it.value);
  } else if (it.kind == UI_PAGE) {
    snprintf(out, cap, "%s", it.label);      // 子頁的箭頭交給繪製端畫
  } else {
    snprintf(out, cap, "%s", it.label);
  }
}

// ---------------------------------------------------------------------------
const char *Ui::feed(UiKey k) {
  if (!_pages || k == UI_KEY_NONE) return nullptr;

  // 被狀態畫面佔用時（演奏中、採樣中…），只有 BACK 有作用：叫醒選單。
  // 其餘按鍵讓給那個模式自己處理，避免「演奏到一半誤觸選單」。
  if (_suspended) {
    if (k == UI_KEY_BACK) _suspended = false;
    return nullptr;
  }

  const UiPage &p = _pages[_page];

  // ---- 數值編輯模式 ------------------------------------------------------
  if (_editing) {
    const UiItem *it = cur();
    if (!it || it->kind != UI_ADJUST || !it->value) { _editing = false; return nullptr; }

    if (k == UI_KEY_OK || k == UI_KEY_BACK) {
      _editing = false;
      // 離開時才送指令：不要每按一下就送一次，否則像 micGain 這種會洗版，
      // 而且中間的值本來就沒意義。
      snprintf(_cmdBuf, sizeof(_cmdBuf), it->cmd ? it->cmd : "", (int)*it->value);
      return _cmdBuf;
    }
    int v = *it->value;
    if (k == UI_KEY_UP)   v += it->vstep;
    if (k == UI_KEY_DOWN) v -= it->vstep;
    if (v < it->vmin) v = it->vmin;
    if (v > it->vmax) v = it->vmax;
    *it->value = (int16_t)v;
    return nullptr;
  }

  // ---- 一般導航 ----------------------------------------------------------
  switch (k) {
    case UI_KEY_UP:
      if (p.n) _cursor = (_cursor == 0) ? (uint8_t)(p.n - 1) : (uint8_t)(_cursor - 1);
      clampScroll();
      return nullptr;

    case UI_KEY_DOWN:
      if (p.n) _cursor = (uint8_t)((_cursor + 1) % p.n);
      clampScroll();
      return nullptr;

    case UI_KEY_BACK: {
      if (_depth > 0) {
        _depth--;
        _page   = _stackPage[_depth];
        _cursor = _stackCur[_depth];
        clampScroll();
      }
      return nullptr;
    }

    case UI_KEY_OK: {
      const UiItem *it = cur();
      if (!it) return nullptr;

      if (it->kind == UI_PAGE) {
        if (it->target >= _nPages) return nullptr;
        if (_depth < UI_MAX_DEPTH) {
          _stackPage[_depth] = _page;
          _stackCur[_depth]  = _cursor;
          _depth++;
        }
        _page = it->target;
        _cursor = 0;
        _top = 0;
        return nullptr;
      }

      if (it->kind == UI_ADJUST) { _editing = true; return nullptr; }

      // UI_CMD
      if (!it->cmd) return nullptr;
      snprintf(_cmdBuf, sizeof(_cmdBuf), "%s", it->cmd);
      return _cmdBuf;
    }

    default:
      return nullptr;
  }
}

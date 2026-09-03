// ============================================================================
//  ui.h  -  4 顆按鈕 + OLED 選單，取代序列埠操作
//
//  接線（每顆按鈕一腳接對應 Teensy 腳位，另一腳接 GND；用內部上拉，不用電阻）
//
//        按鈕        腳位     說明
//        上 UP        2      選單往上；數值 +1（按住會連發）
//        下 DOWN      3      選單往下；數值 -1（按住會連發）
//        確定 OK      4      進入子選單 / 執行 / 進入數值編輯
//        返回 BACK    5      回上一層 / 離開數值編輯 / 從狀態畫面回選單
//
//  腳位 2、3 沿用原本的錄音/演奏按鈕，所以那兩顆線不用重接，只是意義變了。
//  4 和 5 是新增的。Teensy 4.1 上 4、5、14、16、17、24~33 都沒被 audio shield
//  佔用，要改腳位改 config.h 即可。
//
//  --- 設計上的兩個決定 -------------------------------------------------------
//
//  1) 選單項目「不自己做事」，只回傳一個指令字串，交給 .ino 的 handleCommand()
//     執行。錄音、分析、訓練那些流程因此只有一份實作，序列埠也維持完全可用。
//     否則同一個功能會有兩套程式碼，改了一邊忘了另一邊。
//
//  2) 這個檔案不碰 Arduino 的任何東西（沒有 digitalRead、沒有 u8g2），
//     導航邏輯純粹是狀態機。所以桌機上跑得動，可以寫測試 ——
//     「按下去有沒有跑到對的地方」這種事不該靠燒錄進去用眼睛看。
// ============================================================================
#pragma once

#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------- 按鍵 ------
enum UiKey : uint8_t {
  UI_KEY_NONE = 0,
  UI_KEY_UP,
  UI_KEY_DOWN,
  UI_KEY_OK,
  UI_KEY_BACK
};

// ------------------------------------------------------------ 選單資料 ------
enum UiKind : uint8_t {
  UI_PAGE   = 0,   // 進入子頁
  UI_CMD    = 1,   // 執行指令
  UI_ADJUST = 2    // 數值編輯：進去之後上下改值，返回離開
};

struct UiItem {
  const char *label;
  UiKind      kind;
  uint8_t     target;      // UI_PAGE：子頁編號
  const char *cmd;         // UI_CMD：指令字串；UI_ADJUST：含一個 %d 的格式
  int16_t     vmin, vmax, vstep;
  int16_t    *value;       // UI_ADJUST：實際變數（由 .ino 提供）
};

struct UiPage {
  const char   *title;
  const UiItem *items;
  uint8_t       n;
  uint8_t       parent;    // 返回時去哪一頁；根頁指向自己
};

// ------------------------------------------------------------------ Ui -----
#define UI_VISIBLE_ROWS 4        // 128x64 扣掉標題與底線之後放得下 4 列
#define UI_MAX_DEPTH    4

class Ui {
public:
  void begin(const UiPage *pages, uint8_t nPages);

  // 餵一個按鍵進來。回傳要執行的指令字串；沒有就回 nullptr。
  // 回傳的指標指向內部緩衝區，呼叫端要馬上用掉（handleCommand 會複製）。
  const char *feed(UiKey k);

  // --- 給繪製用 -----------------------------------------------------------
  const char *title() const;
  uint8_t     rowCount() const;                 // 這一頁有幾個項目
  uint8_t     cursor() const { return _cursor; }
  uint8_t     topRow() const { return _top; }
  // 第 row 個項目要顯示的文字（數值項會帶上目前的值）
  void        rowText(uint8_t row, char *out, size_t cap) const;
  bool        editing() const { return _editing; }

  // 狀態畫面（演奏中、分析中…）時選單要讓位；結束後再叫醒
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

  // 返回用的堆疊：記住每一層的游標位置，回去時停在原本那一項
  uint8_t _stackPage[UI_MAX_DEPTH];
  uint8_t _stackCur[UI_MAX_DEPTH];
  uint8_t _depth = 0;

  char _cmdBuf[32];

  const UiItem *cur() const;
  void  clampScroll();
};

extern Ui gUi;

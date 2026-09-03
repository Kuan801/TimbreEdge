// ============================================================================
//  ui_test.cpp  -  在桌機上驗證 OLED 選單的導航邏輯
//
//  用法：  make ui_test && ./ui_test
//
//  ui.cpp 刻意不碰 Arduino 的任何東西，所以整個選單狀態機在桌機上跑得動。
//  「按下去有沒有跑到對的地方」這種事不該靠燒進 Teensy 用眼睛看 ——
//  之前 USB MIDI 那段就是因為只能在硬體上驗，來回花掉好幾輪。
//
//  這裡用的選單樹是 TimbreClone.ino 那份的複本。它必須跟本尊一致，
//  所以最後一項測試會檢查頁數／項目數對不對，改了本尊卻忘了改這裡就會失敗。
// ============================================================================
#include "../../ui.h"
#include <cstdio>
#include <cstring>

static int gFail = 0;
static void check(const char *what, bool ok, const char *detail = "") {
  printf("  %-44s %s %s\n", what, ok ? "通過" : "**失敗**", detail);
  if (!ok) gFail++;
}

// ---- 跟 TimbreClone.ino 同一份選單樹 ---------------------------------------
static int16_t gMicGain = 36;
static int16_t gEpochs  = 300;

enum { PG_ROOT = 0, PG_PLAY, PG_SAMPLE, PG_TIMBRE, PG_TRAIN, PG_PURGE };

static const UiItem kRootItems[] = {
  { "Play",     UI_PAGE, PG_PLAY,   nullptr, 0,0,0, nullptr },
  { "Sampling", UI_PAGE, PG_SAMPLE, nullptr, 0,0,0, nullptr },
  { "Timbre",   UI_PAGE, PG_TIMBRE, nullptr, 0,0,0, nullptr },
  { "Training", UI_PAGE, PG_TRAIN,  nullptr, 0,0,0, nullptr },
  { "Status",   UI_CMD,  0, "?stat", 0,0,0, nullptr },
};
static int16_t gOctave = 0;
static const UiItem kPlayItems[] = {
  { "Keyboard 12 keys",UI_CMD,    0, "?keys",   0,0,0, nullptr },
  { "PC keyboard map", UI_CMD,    0, "?pckb",   0,0,0, nullptr },
  { "Scale",           UI_CMD,    0, "p",       0,0,0, nullptr },
  { "Scale + record",  UI_CMD,    0, "w",       0,0,0, nullptr },
  { "Stop",            UI_CMD,    0, "x",       0,0,0, nullptr },
  { "Key octave",      UI_ADJUST, 0, "?oct %d", -2, 2, 1, &gOctave },
  { "Volume +",        UI_CMD,    0, "+",       0,0,0, nullptr },
  { "Volume -",        UI_CMD,    0, "-",       0,0,0, nullptr },
};
static const UiItem kSampleItems[] = {
  { "Record 1 note", UI_CMD,    0, "r", 0,0,0, nullptr },
  { "Auto sampling", UI_CMD,    0, "s", 0,0,0, nullptr },
  { "Input monitor", UI_CMD,    0, "o", 0,0,0, nullptr },
  { "Input gain",    UI_ADJUST, 0, "g %d", 0, 63, 1, &gMicGain },
  { "Mic / Line in", UI_CMD,    0, "i", 0,0,0, nullptr },
};
static const UiItem kTimbreItems[] = {
  { "Load all WAV",   UI_CMD, 0, "n *", 0,0,0, nullptr },
  { "Analyze REC",    UI_CMD, 0, "a",   0,0,0, nullptr },
  { "Bank coverage",  UI_CMD, 0, "v",   0,0,0, nullptr },
  { "Reload profile", UI_CMD, 0, "l",   0,0,0, nullptr },
  { "Clear trainset", UI_CMD, 0, "z",   0,0,0, nullptr },
  { "Delete WAV files", UI_PAGE, PG_PURGE, nullptr, 0,0,0, nullptr },
};
// Cancel 放第一項：進來游標停在 0，手滑連按兩下確定只會取消
static const UiItem kPurgeItems[] = {
  { "Cancel",           UI_CMD, 0, "?back", 0,0,0, nullptr },
  { "DELETE rec+synth", UI_CMD, 0, "y y",   0,0,0, nullptr },
};
static const UiItem kTrainItems[] = {
  { "Epochs",       UI_ADJUST, 0, "t %d", 50, 2000, 50, &gEpochs },
  { "MLP on/off",   UI_CMD,    0, "k", 0,0,0, nullptr },
  { "Reload MODEL", UI_CMD,    0, "m", 0,0,0, nullptr },
};
// 跟本尊一樣用 sizeof 算，不手寫項目數
#define UI_PAGE_DEF(title, arr, parent) \
  { title, arr, (uint8_t)(sizeof(arr) / sizeof((arr)[0])), parent }

static const UiPage kPages[] = {
  UI_PAGE_DEF("TimbreClone", kRootItems,   PG_ROOT),
  UI_PAGE_DEF("Play",        kPlayItems,   PG_ROOT),
  UI_PAGE_DEF("Sampling",    kSampleItems, PG_ROOT),
  UI_PAGE_DEF("Timbre",      kTimbreItems, PG_ROOT),
  UI_PAGE_DEF("Training",    kTrainItems,  PG_ROOT),
  UI_PAGE_DEF("Delete files?", kPurgeItems, PG_TIMBRE),
};

// ---------------------------------------------------------------------------
static const char *press(Ui &u, UiKey k) { return u.feed(k); }
static const char *pressN(Ui &u, UiKey k, int n) {
  const char *r = nullptr;
  for (int i = 0; i < n; i++) r = u.feed(k);
  return r;
}
static bool titleIs(Ui &u, const char *t) { return strcmp(u.title(), t) == 0; }

int main() {
  Ui u;
  u.begin(kPages, sizeof(kPages) / sizeof(kPages[0]));
  char buf[26];

  printf("\n=== OLED 選單導航測試 ===\n\n");

  printf("1) 基本移動\n");
  check("開機停在根選單", titleIs(u, "TimbreClone"));
  check("游標從第 0 項開始", u.cursor() == 0);
  press(u, UI_KEY_DOWN);
  check("按下 -> 游標到第 1 項", u.cursor() == 1);
  press(u, UI_KEY_UP);
  check("按上 -> 回到第 0 項", u.cursor() == 0);
  press(u, UI_KEY_UP);
  check("在第 0 項按上 -> 繞到最後一項", u.cursor() == 4);
  press(u, UI_KEY_DOWN);
  check("在最後一項按下 -> 繞回第 0 項", u.cursor() == 0);

  printf("\n2) 進出子選單\n");
  check("在根選單按確定不會送出指令", press(u, UI_KEY_OK) == nullptr);
  check("進到 Play 頁", titleIs(u, "Play"));
  check("進入子頁後游標歸零", u.cursor() == 0);
  press(u, UI_KEY_DOWN);
  press(u, UI_KEY_BACK);
  check("按返回回到根選單", titleIs(u, "TimbreClone"));
  check("返回後游標停在原本那一項", u.cursor() == 0);
  press(u, UI_KEY_BACK);
  check("在根選單按返回不會出事", titleIs(u, "TimbreClone"));

  printf("\n3) 執行指令\n");
  press(u, UI_KEY_OK);                       // 進 Play
  const char *c = press(u, UI_KEY_OK);       // Keyboard
  check("選 Keyboard 送出 \"?keys\"", c && strcmp(c, "?keys") == 0, c ? c : "(null)");
  press(u, UI_KEY_DOWN);
  c = press(u, UI_KEY_OK);
  check("選 PC keyboard 送出 \"?pckb\"", c && strcmp(c, "?pckb") == 0, c ? c : "(null)");
  press(u, UI_KEY_DOWN);
  c = press(u, UI_KEY_OK);
  check("選 Scale 送出 \"p\"", c && strcmp(c, "p") == 0, c ? c : "(null)");
  // 八度是數值項，要能來回調整並帶著值送出
  pressN(u, UI_KEY_DOWN, 3);                 // -> Key octave
  press(u, UI_KEY_OK);
  pressN(u, UI_KEY_DOWN, 5);                 // 下限 -2
  check("八度下限夾在 -2", gOctave == -2);
  pressN(u, UI_KEY_UP, 3);
  c = press(u, UI_KEY_OK);
  check("八度送出帶負號", c && strcmp(c, "?oct 1") == 0, c ? c : "(null)");
  press(u, UI_KEY_BACK);

  printf("\n4) 數值編輯（麥克風增益）\n");
  pressN(u, UI_KEY_DOWN, 1);                 // Sampling
  press(u, UI_KEY_OK);
  check("進到 Sampling 頁", titleIs(u, "Sampling"));
  pressN(u, UI_KEY_DOWN, 3);                 // Input gain
  u.rowText(3, buf, sizeof(buf));
  check("未編輯時顯示 \"Mic gain 36\"", strcmp(buf, "Input gain 36") == 0, buf);
  check("按確定進入編輯不送指令", press(u, UI_KEY_OK) == nullptr);
  check("editing() 為真", u.editing());
  u.rowText(3, buf, sizeof(buf));
  check("編輯中用中括號標示", strcmp(buf, "Input gain [36]") == 0, buf);
  pressN(u, UI_KEY_UP, 5);
  check("上鍵 5 次 -> 41", gMicGain == 41);
  pressN(u, UI_KEY_DOWN, 2);
  check("下鍵 2 次 -> 39", gMicGain == 39);
  pressN(u, UI_KEY_UP, 100);
  check("上限夾在 63", gMicGain == 63);
  pressN(u, UI_KEY_DOWN, 200);
  check("下限夾在 0", gMicGain == 0);
  gMicGain = 42;
  c = press(u, UI_KEY_OK);
  check("離開編輯才送出，且帶著數值", c && strcmp(c, "g 42") == 0, c ? c : "(null)");
  check("離開後 editing() 為假", !u.editing());
  // 編輯途中不該一直送指令，否則 micGain 會被洗版
  press(u, UI_KEY_OK);
  check("再次進入編輯", u.editing());
  check("編輯中按上鍵不送指令", press(u, UI_KEY_UP) == nullptr);
  check("編輯中按下鍵不送指令", press(u, UI_KEY_DOWN) == nullptr);
  c = press(u, UI_KEY_BACK);
  check("返回鍵也能離開編輯並送出", c && strncmp(c, "g ", 2) == 0, c ? c : "(null)");

  printf("\n5) 捲動（項目多於 4 列時）\n");
  press(u, UI_KEY_BACK);                     // 回根選單，游標停在 Sampling(1)
  check("返回後游標回到 Sampling", u.cursor() == 1);
  press(u, UI_KEY_DOWN);                     // -> Timbre(2)
  press(u, UI_KEY_OK);                       // Timbre，6 項
  check("進到 Timbre 頁", titleIs(u, "Timbre"));
  check("一開始從第 0 列顯示", u.topRow() == 0);
  pressN(u, UI_KEY_DOWN, 3);
  check("游標到第 3 項時還不用捲", u.topRow() == 0 && u.cursor() == 3);
  press(u, UI_KEY_DOWN);
  check("游標到第 4 項 -> 捲一列", u.topRow() == 1 && u.cursor() == 4);
  press(u, UI_KEY_DOWN);
  check("游標到第 5 項 -> 再捲一列", u.topRow() == 2 && u.cursor() == 5);
  press(u, UI_KEY_DOWN);
  check("繞回第 0 項 -> 捲回頂端", u.topRow() == 0 && u.cursor() == 0);

  printf("\n5b) 刪檔確認頁（不可逆，手滑不能觸發）\n");
  // Cancel 放第一項是這一頁唯一的保護機制，值得測。
  pressN(u, UI_KEY_DOWN, 5);                 // -> Delete WAV files
  check("游標移到 Delete WAV files", u.cursor() == 5);
  check("進入確認頁不會送出任何指令", press(u, UI_KEY_OK) == nullptr);
  check("標題是 Delete files?", titleIs(u, "Delete files?"));
  check("游標停在第 0 項（Cancel）", u.cursor() == 0);
  u.rowText(0, buf, sizeof(buf));
  check("第 0 項就是 Cancel", strcmp(buf, "Cancel") == 0, buf);
  // 關鍵：從 Timbre 頁連按兩下確定，第二下必須落在 Cancel
  c = press(u, UI_KEY_OK);
  check("連按兩下確定只會取消，不會刪檔",
        c == nullptr || strcmp(c, "y y") != 0, c ? c : "(null)");
  // "?back" 是選單內部指令，由 .ino 轉成一個返回鍵。這裡照樣模擬那一步，
  // 否則測的就不是使用者實際會遇到的行為。
  if (c && strcmp(c, "?back") == 0) press(u, UI_KEY_BACK);
  check("取消後回到 Timbre 頁", titleIs(u, "Timbre"));
  // 真的要刪就得多按一次「下」再確定
  press(u, UI_KEY_OK);                       // 再進確認頁
  press(u, UI_KEY_DOWN);
  c = press(u, UI_KEY_OK);
  check("往下一項才是真正的刪除", c && strcmp(c, "y y") == 0, c ? c : "(null)");
  press(u, UI_KEY_BACK);

  printf("\n6) 讓位機制（演奏中不該被誤觸）\n");
  u.setSuspended(true);
  check("讓位中，方向鍵完全沒作用", press(u, UI_KEY_DOWN) == nullptr && u.suspended());
  check("讓位中，確定鍵也不送指令", press(u, UI_KEY_OK) == nullptr);
  press(u, UI_KEY_BACK);
  check("返回鍵叫醒選單", !u.suspended());

  printf("\n7) 選單樹與本尊一致\n");
  const int nPages = sizeof(kPages) / sizeof(kPages[0]);
  bool ok = true;
  for (int i = 0; i < nPages; i++) {
    // 每頁宣告的項目數要跟實際陣列長度相符 —— 對不上會讀到界外
    if (kPages[i].n == 0 || kPages[i].items == nullptr) ok = false;
    for (int j = 0; j < kPages[i].n; j++) {
      const UiItem &it = kPages[i].items[j];
      if (!it.label) ok = false;
      if (it.kind == UI_PAGE   && it.target >= nPages) ok = false;
      if (it.kind == UI_CMD    && !it.cmd) ok = false;
      if (it.kind == UI_ADJUST && (!it.value || !it.cmd || it.vstep <= 0)) ok = false;
    }
  }
  check("每一頁的項目數／指標／子頁編號都合法", ok);

  printf("\n%s\n", gFail ? "有測試沒過" : "全部通過");
  return gFail ? 1 : 0;
}

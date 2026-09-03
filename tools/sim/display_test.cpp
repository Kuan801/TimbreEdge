// ============================================================================
//  display_test.cpp  -  驗證「選單不會蓋掉狀態面板」
//
//  用法：  make display_test && ./display_test
//
//  這條規則壞掉的表現非常難查：螢幕一直在更新，看起來不像當機，但按了
//  Auto sampling 之後採樣面板一瞬間就被選單蓋回去，電平表永遠出不來 ——
//  使用者只會說「按了沒反應」。真正的線索是「螢幕還在更新」：
//  那就不是當機，是畫錯東西。
//
//  displaySetState() / displaySetMenu() 的狀態邏輯都在 #if TC_USE_OLED 之外，
//  所以 TC_USE_OLED=0 時整段規則在桌機上驗得到，不必接 OLED。
// ============================================================================
#include "Arduino.h"
#include "../../display.h"
#include <cstdio>

static int gFail = 0;
static void check(const char *what, bool ok) {
  printf("  %-54s %s\n", what, ok ? "通過" : "**失敗**");
  if (!ok) gFail++;
}

static const char kRows[4][26] = { "Row A", "Row B", "Row C", "Row D" };

// 選單想開就開得起來嗎？回傳「呼叫之後選單到底有沒有顯示」
static bool tryOpenMenu() {
  displaySetMenu("Menu", kRows, 4, 0, 0, 5, false);
  return displayMenuVisible();
}

int main() {
  printf("\n=== OLED 選單 / 狀態面板 優先權測試 ===\n\n");

  printf("1) displayState() 如實回報\n");
  displaySetState(TC_ST_IDLE);
  check("設成 IDLE 讀回 IDLE", displayState() == TC_ST_IDLE);
  displaySetState(TC_ST_RECORDING, "SAMPLING");
  check("設成 RECORDING 讀回 RECORDING", displayState() == TC_ST_RECORDING);

  printf("\n2) 進入忙碌狀態會自動把選單推開\n");
  displaySetState(TC_ST_IDLE);
  check("閒置時選單開得起來", tryOpenMenu());
  displaySetState(TC_ST_RECORDING, "SAMPLING");
  check("一進入 RECORDING，選單就自動關掉", !displayMenuVisible());

  printf("\n3) 忙碌狀態下開選單要被擋下來\n");
  // 這是 Auto sampling 那個 bug 的最小重現：
  //    handleCommand("s")  -> displaySetState(RECORDING)   面板出現
  //    uiHandleKey 結尾    -> displaySetMenu(...)          <- 就是這一步蓋掉面板
  // 沒有這道防線的話，下面四項全都會是「選單開起來了」。
  displaySetState(TC_ST_RECORDING, "SAMPLING - play a note");
  check("採樣中開不了選單", !tryOpenMenu());
  displaySetState(TC_ST_ANALYZING, "REC.WAV");
  check("分析中開不了選單", !tryOpenMenu());
  displaySetState(TC_ST_TRAINING);
  check("訓練中開不了選單", !tryOpenMenu());
  displaySetState(TC_ST_PLAYING);
  check("演奏中開不了選單", !tryOpenMenu());
  displaySetState(TC_ST_ERROR, "oops");
  check("錯誤畫面上開不了選單", !tryOpenMenu());

  printf("\n4) 回到閒置之後選單要能正常開\n");
  // 擋過頭比不擋更慘 —— 選單再也回不來，就真的只能重開機了
  displaySetState(TC_ST_IDLE);
  check("回到 IDLE 選單又開得起來", tryOpenMenu());
  displaySetState(TC_ST_BOOT);
  check("BOOT 時也開得起來（不然一開機就沒選單）", tryOpenMenu());

  printf("\n5) 傳 nullptr 是「關掉選單」，任何狀態下都要接受\n");
  displaySetState(TC_ST_IDLE);
  tryOpenMenu();
  displaySetMenu(nullptr, nullptr, 0, 0, 0, 0, false);
  check("nullptr 關得掉選單", !displayMenuVisible());

  printf("\n6) 電平表的資料設定完再開選單，選單仍然被擋\n");
  // 採樣迴圈每 150 ms 就會設一次電平表然後 displayService()。
  // 這一節確認那個流程不會意外把選單放回來。
  displaySetState(TC_ST_RECORDING, "SAMPLING");
  displaySetLine(0, "########........");
  displaySetProgress(0.5f);
  check("設過電平表之後選單仍開不起來", !tryOpenMenu());
  check("狀態沒有被選單呼叫改掉", displayState() == TC_ST_RECORDING);

  printf("\n%s\n", gFail ? "有測試沒過" : "全部通過");
  return gFail ? 1 : 0;
}

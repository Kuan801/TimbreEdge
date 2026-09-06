#include "Arduino.h"
#include "../../display.h"
#include <cstdio>

static int gFail = 0;
static void check(const char *what, bool ok) {
  printf("  %-54s %s\n", what, ok ? "通過" : "**失敗**");
  if (!ok) gFail++;
}

static const char kRows[4][26] = { "Row A", "Row B", "Row C", "Row D" };

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

  displaySetState(TC_ST_RECORDING, "SAMPLING");
  displaySetLine(0, "########........");
  displaySetProgress(0.5f);
  check("設過電平表之後選單仍開不起來", !tryOpenMenu());
  check("狀態沒有被選單呼叫改掉", displayState() == TC_ST_RECORDING);

  printf("\n%s\n", gFail ? "有測試沒過" : "全部通過");
  return gFail ? 1 : 0;
}

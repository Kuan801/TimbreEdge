// ============================================================================
//  wavname_test.cpp  -  「哪些檔案可以刪」的判斷
//
//  用法：  make wavname_test && ./wavname_test
//
//  這是整個專案裡最不能出錯的一段邏輯：判斷錯了就會刪掉使用者辛苦錄的素材，
//  而且不可逆。所以規則刻意訂得嚴：
//
//    可以刪：REC.WAV、CANON.WAV、以及「純音名」的檔（C4.WAV、Db4.WAV…）
//    不能刪：其他一律不動
//
//  使用者放的素材都有前綴（Piano.mf.C4.wav、Trumpet.vib.ff.C4.stereo.wav），
//  不會落進「純音名」那一類。寧可漏刪幾個檔，也不要誤刪一個。
// ============================================================================
#include "Arduino.h"
#include "../../wav_io.h"
#include <cstdio>

static int gFail = 0;
static void yes(const char *nm) {
  const bool ok = tcIsGeneratedWav(nm);
  printf("  %-34s 應該刪 -> %s\n", nm, ok ? "刪 通過" : "不刪 **失敗**");
  if (!ok) gFail++;
}
static void no(const char *nm) {
  const bool ok = !tcIsGeneratedWav(nm);
  printf("  %-34s 不能刪 -> %s\n", nm, ok ? "不刪 通過" : "刪 **失敗**");
  if (!ok) gFail++;
}

int main() {
  printf("\n=== 可刪檔名判斷 ===\n\n");

  printf("1) 程式產生的固定檔名\n");
  yes("REC.WAV");
  yes("CANON.WAV");
  yes("rec.wav");            // 大小寫不該影響（FAT 本來就不分）
  yes("Canon.Wav");

  printf("\n2) 採樣模式自動命名的音名檔\n");
  yes("C4.WAV");
  yes("Db4.WAV");
  yes("A3.WAV");
  yes("Gb2.WAV");
  yes("B0.WAV");
  yes("C10.WAV");            // 兩位數八度
  yes("c4.wav");
  yes("A#4.WAV");            // 有些命名法用 #

  printf("\n3) 使用者自己放的素材（絕對不能刪）\n");
  no("Piano.mf.C4.wav");
  no("Trumpet.vib.ff.C4.stereo.wav");
  no("Violin.A4.wav");
  no("Flute.ff.Ab4.stereo.wav");
  no("MySample.wav");
  no("C4_take2.wav");
  no("C4 copy.wav");
  no("backup_C4.WAV");

  printf("\n4) 非 WAV 一律不碰\n");
  no("BANK.BIN");            // 音色庫是另一回事，要用 Clear trainset
  no("PROFILE.BIN");
  no("MODEL.BIN");
  no("FRAMES.CSV");
  no("C4.TXT");
  no("C4.WAVE");             // 副檔名不是剛好 .WAV
  no("README.md");

  printf("\n5) 邊界與畸形輸入\n");
  no(nullptr);
  no("");
  no(".WAV");                // 沒有音名
  no("H4.WAV");              // H 不是音名（A~G 而已）
  no("C.WAV");               // 沒有八度數字
  no("C444.WAV");            // 三位數八度，不是我們產生的
  no("Cb.WAV");
  no("4C.WAV");              // 顛倒
  no("CC4.WAV");             // 兩個字母
  no("C4x.WAV");             // 數字後面還有東西

  printf("\n%s\n", gFail ? "有測試沒過" : "全部通過");
  return gFail ? 1 : 0;
}

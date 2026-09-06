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
  yes("rec.wav");
  yes("Canon.Wav");

  printf("\n2) 採樣模式自動命名的音名檔\n");
  yes("C4.WAV");
  yes("Db4.WAV");
  yes("A3.WAV");
  yes("Gb2.WAV");
  yes("B0.WAV");
  yes("C10.WAV");
  yes("c4.wav");
  yes("A#4.WAV");

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
  no("BANK.BIN");
  no("PROFILE.BIN");
  no("MODEL.BIN");
  no("FRAMES.CSV");
  no("C4.TXT");
  no("C4.WAVE");
  no("README.md");

  printf("\n5) 邊界與畸形輸入\n");
  no(nullptr);
  no("");
  no(".WAV");
  no("H4.WAV");
  no("C.WAV");
  no("C444.WAV");
  no("Cb.WAV");
  no("4C.WAV");
  no("CC4.WAV");
  no("C4x.WAV");

  printf("\n%s\n", gFail ? "有測試沒過" : "全部通過");
  return gFail ? 1 : 0;
}

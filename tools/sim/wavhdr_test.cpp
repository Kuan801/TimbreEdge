#include "../../wav_io.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

static int gFail = 0;
static void check(const char *what, bool ok, const char *detail = "") {
  printf("  %-48s %s %s\n", what, ok ? "通過" : "**失敗**", detail);
  if (!ok) gFail++;
}

struct Hdr { bool riff; uint32_t riffSize, dataSize; uint32_t fileSize; };

static Hdr readHdr(const char *path) {
  Hdr h = {false, 0, 0, 0};
  std::string full = std::string("./") + path;
  FILE *f = fopen(full.c_str(), "rb");
  if (!f) return h;
  fseek(f, 0, SEEK_END);
  h.fileSize = (uint32_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t b[48];
  if (fread(b, 1, 48, f) == 48) {
    h.riff = (memcmp(b, "RIFF", 4) == 0) && (memcmp(b + 8, "WAVE", 4) == 0);
    memcpy(&h.riffSize, b + 4,  4);
    memcpy(&h.dataSize, b + 40, 4);
  }
  fclose(f);
  return h;
}

static bool playable(const Hdr &h) {
  return h.riff && h.dataSize > 0 && h.dataSize + 44 <= h.fileSize;
}

static int16_t gBuf[512];

int main() {
  for (int i = 0; i < 512; i++) gBuf[i] = (int16_t)(i * 37);

  printf("\n=== 正常收尾（有 close）===\n");
  {
    WavWriter w;
    check("開得起來", w.open("_hdrtest_ok.wav", 44100, 2));
    for (int i = 0; i < 40; i++) w.writeSamples(gBuf, 512);
    w.close();
    Hdr h = readHdr("_hdrtest_ok.wav");
    char m[96];
    snprintf(m, sizeof(m), "(檔案 %u，data %u)", h.fileSize, h.dataSize);
    check("是合法 WAV 而且播得出來", playable(h), m);
    check("data 長度等於實際寫進去的量", h.dataSize == 40u * 512u * 2u);
    check("RIFF 長度 = 檔案大小 - 8", h.riffSize == h.fileSize - 8);
  }

  printf("\n=== 錄到一半斷電（沒有 close）===\n");
  {
    WavWriter w;
    w.open("_hdrtest_cut.wav", 44100, 2);
    for (int i = 0; i < 20; i++) w.writeSamples(gBuf, 512);
    w.flushHeader();
    for (int i = 0; i < 5; i++) w.writeSamples(gBuf, 512);

    Hdr h = readHdr("_hdrtest_cut.wav");
    char m[96];
    snprintf(m, sizeof(m), "(檔案 %u，data %u)", h.fileSize, h.dataSize);
    check("仍然是播得出來的 WAV", playable(h), m);
    check("data 長度是上一次補正時的量（最多損失那 2 秒）",
          h.dataSize == 20u * 512u * 2u);

    check("補正涵蓋的資料確實已經在檔案裡",
          h.fileSize >= 44u + 20u * 512u * 2u);
  }

  printf("\n=== 負對照：如果沒有 flushHeader ===\n");
  {

    WavWriter w;
    w.open("_hdrtest_old.wav", 44100, 2);
    for (int i = 0; i < 20; i++) w.writeSamples(gBuf, 512);

    Hdr h = readHdr("_hdrtest_old.wav");
    char m[96];
    snprintf(m, sizeof(m), "(檔案 %u，data 卻是 %u)", h.fileSize, h.dataSize);
    check("舊行為確實會留下 data=0 的壞檔", !playable(h) && h.dataSize == 0, m);
  }

  printf("\n=== 預先寫入長度的路徑（Recorder 用）===\n");
  {

    WavWriter w;
    w.open("_hdrtest_short.wav", 44100, 1, 44100 * 3);
    for (int i = 0; i < 10; i++) w.writeSamples(gBuf, 512);
    Hdr before = readHdr("_hdrtest_short.wav");
    check("補正前：標頭宣告的比實際資料多（壞檔）",
          before.dataSize > before.fileSize - 44);
    w.flushHeader();
    Hdr after = readHdr("_hdrtest_short.wav");
    check("補正後：變成播得出來的合法 WAV", playable(after));
    w.close();
  }

  remove("./_hdrtest_ok.wav");
  remove("./_hdrtest_cut.wav");
  remove("./_hdrtest_old.wav");
  remove("./_hdrtest_short.wav");

  printf("\n%s\n", gFail ? "有測試沒過" : "全部通過");
  return gFail ? 1 : 0;
}

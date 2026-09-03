// ============================================================================
//  wavhdr_test.cpp  -  WAV 標頭在「錄到一半就斷電」之後還合不合法
//
//  用法：  make wavhdr_test && ./wavhdr_test
//
//  為什麼要有這個測試
//  ------------------
//  WAV 的 RIFF/data 長度本來只在 close() 補正。錄音或演奏還沒結束就斷電、
//  按 reset、或直接把 SD 卡拔起來的話，音訊資料明明已經寫進去好幾 MB，
//  標頭裡的 data 長度卻還是 open() 當初寫的值（StereoCapture 是 0）。
//
//  這種檔案最難查的地方是「有些播放器讀得出來」：
//      Windows 檔案總管 / Media Player -> 判定損毀，不給播
//      ffmpeg / QuickTime / Audacity   -> 自己掃到檔尾，照樣播
//  於是症狀變成「在 Mac 上聽得到，拿到 Windows 就說檔案壞掉」，
//  很容易被當成播放器的問題而不是寫檔的問題。
//
//  所以 WavWriter 多了 flushHeader()，recorder.cpp 每 2 秒呼叫一次。
//  這支測試把「斷電」模擬成「不呼叫 close()」，直接檢查磁碟上的位元組。
// ============================================================================
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

// 直接讀磁碟上的位元組，不透過 WavWriter —— 要驗的就是「檔案本身」
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

// 一個播放器會怎麼判斷這個檔案能不能播：
//   RIFF/WAVE 標記在、data 長度不是 0、而且沒有宣告比實際還多的資料。
//
// 刻意不要求「RIFF 長度剛好等於檔案大小 - 8」：錄製途中補正之後又寫進去的
// 那幾秒，標頭本來就還沒算到，檔案尾端會多出一段沒被宣告的位元組。
// 播放器對這種情況是忽略尾巴照樣播 —— 宣告得比實際「少」是安全的，
// 宣告得比實際「多」才會壞。
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
    w.flushHeader();                       // recorder.cpp 每 2 秒做的就是這件事
    for (int i = 0; i < 5; i++) w.writeSamples(gBuf, 512);
    // 這裡「斷電」：不呼叫 close()，讓檔案就這樣留在卡上
    Hdr h = readHdr("_hdrtest_cut.wav");
    char m[96];
    snprintf(m, sizeof(m), "(檔案 %u，data %u)", h.fileSize, h.dataSize);
    check("仍然是播得出來的 WAV", playable(h), m);
    check("data 長度是上一次補正時的量（最多損失那 2 秒）",
          h.dataSize == 20u * 512u * 2u);
    // 補正之後又寫進去的資料，有多少真的落到卡上要看有沒有被 flush ——
    // 斷電本來就會吃掉還在快取裡的部分。這裡只確認「已經補正過的那一段
    // 一定在」，那才是這個機制保證的事。
    check("補正涵蓋的資料確實已經在檔案裡",
          h.fileSize >= 44u + 20u * 512u * 2u);
  }

  printf("\n=== 負對照：如果沒有 flushHeader ===\n");
  {
    // 這一段刻意重現舊版的行為，證明上面那條檢查真的有在擋東西。
    // 沒有這個負對照的話，「播得出來」有可能只是因為檢查太寬鬆。
    WavWriter w;
    w.open("_hdrtest_old.wav", 44100, 2);
    for (int i = 0; i < 20; i++) w.writeSamples(gBuf, 512);
    // 不 flushHeader、也不 close —— 這就是修正前的斷電結果
    Hdr h = readHdr("_hdrtest_old.wav");
    char m[96];
    snprintf(m, sizeof(m), "(檔案 %u，data 卻是 %u)", h.fileSize, h.dataSize);
    check("舊行為確實會留下 data=0 的壞檔", !playable(h) && h.dataSize == 0, m);
  }

  printf("\n=== 預先寫入長度的路徑（Recorder 用）===\n");
  {
    // Recorder::start 會把預期長度先寫進標頭。錄滿的話剛好，
    // 提早結束的話標頭會宣告得比實際多 —— 那也是壞檔，close/flushHeader 要修掉。
    WavWriter w;
    w.open("_hdrtest_short.wav", 44100, 1, 44100 * 3);   // 宣告 3 秒
    for (int i = 0; i < 10; i++) w.writeSamples(gBuf, 512);   // 只寫 0.12 秒
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

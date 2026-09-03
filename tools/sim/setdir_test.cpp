// ============================================================================
//  setdir_test  -  採樣資料夾（SETnn）的建立、掃描、刪除
//
//  這一段是「不可逆」的程式碼：算錯編號會蓋掉上一輪的素材，刪錯路徑會刪掉
//  使用者自己放的錄音。真機上出錯就回不來了，所以在桌機的 SD 模擬層上先把
//  每一條路徑跑過 —— 包含「不該刪的東西有沒有被刪掉」這種只能用負對照驗的事。
// ============================================================================
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../../config.h"
#include "../../wav_io.h"

static int gFail = 0;
static void check(const char *name, bool ok, const char *note = "") {
  printf("  %-46s %s %s\n", name, ok ? "通過" : "失敗 <<<<", note);
  if (!ok) gFail++;
}

static std::string gRoot;

static void writeFile(const std::string &rel, size_t bytes = 64) {
  FILE *f = fopen((gRoot + "/" + rel).c_str(), "wb");
  if (!f) { printf("     ！寫不出 %s\n", rel.c_str()); return; }
  for (size_t i = 0; i < bytes; i++) fputc('x', f);
  fclose(f);
}
static bool exists(const std::string &rel) {
  struct stat st;
  return stat((gRoot + "/" + rel).c_str(), &st) == 0;
}

int main() {
  printf("\n採樣資料夾 SETnn\n");

  // 每次跑都用乾淨的暫存目錄，不要動到真的素材
  char tmpl[] = "/tmp/tc_setdir_XXXXXX";
  const char *dir = mkdtemp(tmpl);
  if (!dir) { printf("建不出暫存目錄\n"); return 1; }
  gRoot = dir;
  extern std::string sim_sd_root;
  sim_sd_root = gRoot;

  // -------------------------------------------------------------------------
  printf("\n1) 名稱判斷（純字串，最容易寫錯的地方）\n");
  check("SET01 是採樣資料夾",  tcIsSetDir("SET01"));
  check("SET99 是",            tcIsSetDir("SET99"));
  check("set07 是（不分大小寫）", tcIsSetDir("set07"));
  check("SET1 不是（只有一位數）",  !tcIsSetDir("SET1"));
  check("SET001 不是（三位數）",    !tcIsSetDir("SET001"));
  check("SETXX 不是（不是數字）",   !tcIsSetDir("SETXX"));
  check("SETTINGS 不是",            !tcIsSetDir("SETTINGS"));
  check("空字串不是",               !tcIsSetDir(""));
  check("nullptr 不會爆",           !tcIsSetDir(nullptr));

  // -------------------------------------------------------------------------
  printf("\n2) 編號：連號、補洞、上限\n");
  {
    char a[12], b[12], c[12];
    check("第一次拿到 SET01", tcSdMakeNextSet(a, sizeof(a)) && strcmp(a, "SET01") == 0, a);
    check("第二次拿到 SET02", tcSdMakeNextSet(b, sizeof(b)) && strcmp(b, "SET02") == 0, b);
    check("第三次拿到 SET03", tcSdMakeNextSet(c, sizeof(c)) && strcmp(c, "SET03") == 0, c);

    // 中間那個被使用者手動刪掉之後，新的一輪應該補進那個洞，
    // 而不是一路往上跳 —— 不然號碼會有斷層，久了根本認不出誰是誰
    ::rmdir((gRoot + "/SET02").c_str());
    char d[12];
    check("SET02 被刪掉後，下一個補回 SET02",
          tcSdMakeNextSet(d, sizeof(d)) && strcmp(d, "SET02") == 0, d);
  }

  // -------------------------------------------------------------------------
  printf("\n3) 掃描\n");
  {
    // 放幾個「長得有點像但不是」的東西，確認不會被算進去
    SD.mkdir("SETTINGS");
    SD.mkdir("SAMPLES");
    writeFile("SET04");                 // 同名但是檔案，不是資料夾
    writeFile("Piano.mf.C4.wav");

    static char sets[TC_MAX_SCAN_FILES][TC_MAX_NAME_LEN];
    const int n = tcSdCollectSets(&sets[0][0], TC_MAX_SCAN_FILES);
    char msg[96];
    snprintf(msg, sizeof(msg), "(找到 %d 個：%s %s %s)", n,
             n > 0 ? sets[0] : "-", n > 1 ? sets[1] : "-", n > 2 ? sets[2] : "-");
    check("只掃到 3 個真的採樣資料夾", n == 3, msg);
    check("而且是排序過的",
          n == 3 && strcmp(sets[0], "SET01") == 0 &&
                    strcmp(sets[1], "SET02") == 0 &&
                    strcmp(sets[2], "SET03") == 0);
  }

  // -------------------------------------------------------------------------
  printf("\n4) 刪除\n");
  {
    writeFile("SET01/C4.WAV", 1024);
    writeFile("SET01/D4.WAV", 2048);
    writeFile("SET01/BANK.BIN", 512);

    int nf = 0; uint32_t fb = 0;
    const bool ok = tcSdRemoveDir("SET01", &nf, &fb);
    char msg[64]; snprintf(msg, sizeof(msg), "(%d 個檔，%u bytes)", nf, (unsigned)fb);
    check("整個資料夾刪掉", ok, msg);
    check("裡面的檔數與大小都對", nf == 3 && fb == 1024 + 2048 + 512, msg);
    check("資料夾本身不見了", !exists("SET01"));
  }

  // -------------------------------------------------------------------------
  printf("\n5) 負對照：不該刪的東西一個都不能少\n");
  {
    check("使用者自己的素材還在", exists("Piano.mf.C4.wav"));
    check("SETTINGS 沒被當成採樣資料夾刪掉", exists("SETTINGS"));
    check("SAMPLES 還在", exists("SAMPLES"));
    check("同名的檔案 SET04 還在", exists("SET04"));
    check("其他採樣資料夾沒被波及", exists("SET02") && exists("SET03"));
  }

  // -------------------------------------------------------------------------
  printf("\n6) 壞輸入不會炸\n");
  {
    check("刪不存在的資料夾回 false", !tcSdRemoveDir("SET77"));
    check("刪空字串回 false",         !tcSdRemoveDir(""));
    check("刪 nullptr 回 false",      !tcSdRemoveDir(nullptr));
    // 指到一個「是檔案不是資料夾」的名字，絕對不能把它刪掉
    check("指到檔案時回 false",       !tcSdRemoveDir("SET04"));
    check("而且那個檔案還在",         exists("SET04"));
  }

  // -------------------------------------------------------------------------
  printf("\n7) 資料夾裡的音檔仍然掃得到（n SETnn/ 要能用）\n");
  {
    writeFile("SET02/C4.WAV");
    writeFile("SET02/E4.WAV");
    writeFile("SET02/BANK.BIN");
    static char names[TC_MAX_SCAN_FILES][TC_MAX_NAME_LEN];
    const int n = tcSdCollectWavs("SET02", &names[0][0], TC_MAX_SCAN_FILES);
    char msg[64]; snprintf(msg, sizeof(msg), "(%d 個)", n);
    check("只收 .WAV，BANK.BIN 不算", n == 2, msg);
    check("依檔名排序", n == 2 && strcmp(names[0], "C4.WAV") == 0);
  }

  printf(gFail ? "\n有 %d 項失敗\n" : "\n全部通過\n", gFail);
  printf("（暫存目錄 %s，測試不會自己刪，要看內容隨時進去）\n", gRoot.c_str());
  return gFail ? 1 : 0;
}

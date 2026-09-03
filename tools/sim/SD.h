// tools/sim/SD.h  -  用桌機檔案系統假裝成 SD 卡
#pragma once

#include "Arduino.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#define FILE_READ  0
#define FILE_WRITE 1
#define BUILTIN_SDCARD 254

extern std::string sim_sd_root;

class File {
public:
  File() {}
  File(FILE *f, uint32_t sz) : _f(f), _size(sz) {}
  File(DIR *d, const std::string &path) : _dir(d), _path(path) { _isDir = true; }
  operator bool() const { return _f != nullptr || _dir != nullptr || _valid; }

  // --- 目錄走訪（給 tcSdList 用）---
  bool        isDirectory() const { return _isDir; }
  const char *name() const        { return _name; }
  File        openNextFile();

  int  read(uint8_t *b, size_t n) { return _f ? (int)fread(b, 1, n, _f) : -1; }
  int  read()                     { return _f ? fgetc(_f) : -1; }
  size_t write(const uint8_t *b, size_t n) { return _f ? fwrite(b, 1, n, _f) : 0; }
  size_t write(const char *b, size_t n)    { return write((const uint8_t *)b, n); }
  void print(const char *s)  { if (_f) fprintf(_f, "%s", s); }
  void print(int v)          { if (_f) fprintf(_f, "%d", v); }
  void println()             { if (_f) fprintf(_f, "\n"); }
  void println(const char *s){ if (_f) fprintf(_f, "%s\n", s); }
  template <typename... A> void printf(const char *f, A... a) { if (_f) fprintf(_f, f, a...); }

  bool     seek(uint32_t p) { return _f ? fseek(_f, (long)p, SEEK_SET) == 0 : false; }
  uint32_t position()       { return _f ? (uint32_t)ftell(_f) : 0; }
  uint32_t size()           { return _size; }
  int      available()      { return _f ? (int)(_size - position()) : 0; }
  void     flush()          { if (_f) fflush(_f); }
  void     close()          {
    if (_f)   { fclose(_f);     _f   = nullptr; }
    if (_dir) { closedir(_dir); _dir = nullptr; }
  }

private:
  FILE       *_f     = nullptr;
  uint32_t    _size  = 0;
  DIR        *_dir   = nullptr;
  bool        _isDir = false;
  bool        _valid = false;
  std::string _path;
  char        _name[256] = {0};
};

class SDClass {
public:
  bool begin(int = 0) { return true; }
  bool exists(const char *p) { struct stat st; return stat(full(p).c_str(), &st) == 0; }
  bool remove(const char *p) { return ::remove(full(p).c_str()) == 0; }
  // Teensy 的 SD 函式庫有這兩個，桌機補上同名的包裝，
  // 採樣資料夾（SETnn）那段程式碼才能在這裡跑測試。
  bool mkdir(const char *p) { return ::mkdir(full(p).c_str(), 0755) == 0; }
  bool rmdir(const char *p) { return ::rmdir(full(p).c_str()) == 0; }
  File open(const char *p, int mode = FILE_READ) {
    std::string fp = full(p);
    struct stat st;
    if (stat(fp.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      DIR *d = opendir(fp.c_str());
      if (!d) return File();
      return File(d, fp);
    }
    if (mode == FILE_READ) {
      FILE *f = fopen(fp.c_str(), "rb");
      if (!f) return File();
      fseek(f, 0, SEEK_END);
      uint32_t sz = (uint32_t)ftell(f);
      fseek(f, 0, SEEK_SET);
      return File(f, sz);
    }
    FILE *f = fopen(fp.c_str(), "r+b");
    if (!f) f = fopen(fp.c_str(), "w+b");
    return File(f, 0);
  }
private:
  // 路徑組合。三種情況都要對：
  //   sim_sd_root = "."   一般模擬（把當前目錄當成 SD 卡的根）
  //   sim_sd_root = ""    工具程式吃命令列給的真實路徑
  //   p 本身就是絕對路徑  不管 root 是什麼都直接用
  //
  // 原本無條件寫 root + "/" + p，於是 root 為空時 "a.wav" 會變成 "/a.wav"，
  // 相對路徑全部解析到檔案系統根目錄 —— 症狀是「檔案明明在卻找不到」。
  static std::string full(const char *p) {
    if (!p || !p[0]) return sim_sd_root.empty() ? std::string(".") : sim_sd_root;
    // root 為空 = 工具程式，命令列給什麼路徑就用什麼（相對或絕對都行）
    if (sim_sd_root.empty()) return std::string(p);
    // root 非空 = 模擬 SD 卡。這時候開頭的 "/" 指的是「SD 卡的根目錄」，
    // 不是檔案系統的根 —— 一度把它當成絕對路徑放行，結果 tcSdCollectSets("/")
    // 跑去掃真正的檔案系統根目錄，一個採樣資料夾都找不到。
    return sim_sd_root + "/" + p;
  }
};
extern SDClass SD;

inline File File::openNextFile() {
  if (!_dir) return File();
  struct dirent *de;
  while ((de = readdir(_dir)) != nullptr) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
    std::string fp = _path + "/" + de->d_name;
    struct stat st;
    if (stat(fp.c_str(), &st) != 0) continue;
    File out;
    out._valid = true;
    if (S_ISDIR(st.st_mode)) {
      out._isDir = true;
    } else {
      out._f    = fopen(fp.c_str(), "rb");
      out._size = (uint32_t)st.st_size;
    }
    snprintf(out._name, sizeof(out._name), "%s", de->d_name);
    return out;
  }
  return File();
}

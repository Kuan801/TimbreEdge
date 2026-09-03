#include "recorder.h"

// ============================================================================
//  StereoCapture
// ============================================================================
bool StereoCapture::start(const char *path) {
  if (_active || !_qL || !_qR) return false;
  if (!_w.open(path, (uint32_t)TC_SAMPLE_RATE, 2)) return false;

  _frames  = 0;
  _dropped = 0;
  _hdrAt   = 0;
  _fill    = 0;
  _active  = true;

  // 先清掉佇列裡的殘留，確保左右聲道從同一個 block 對齊開始
  _qL->begin();
  _qR->begin();
  while (_qL->available()) { _qL->readBuffer(); _qL->freeBuffer(); }
  while (_qR->available()) { _qR->readBuffer(); _qR->freeBuffer(); }

  Serial.printf("[CAP] 開始錄製演奏 -> %s (44.1k 立體聲)\n", path);
  return true;
}

void StereoCapture::service() {
  if (!_active) return;

  // 兩邊都有資料才取，否則左右聲道會錯開
  while (_qL->available() > 0 && _qR->available() > 0) {
    int16_t *l = _qL->readBuffer();
    int16_t *r = _qR->readBuffer();
    for (int i = 0; i < TC_BLOCK; i++) {
      _buf[_fill++] = l[i];
      _buf[_fill++] = r[i];
    }
    _qL->freeBuffer();
    _qR->freeBuffer();
    _frames += TC_BLOCK;

    if (_fill >= 512) {
      if (!_w.writeSamples(_buf, 512)) _dropped++;
      _fill = 0;
    }

    // 每 2 秒把標頭的長度補到目前為止。沒有這一段的話，錄到一半斷電
    // 留下的檔案 data 長度是 0，Windows 會直接說檔案損毀 —— 即使裡面
    // 已經有好幾 MB 的音訊（ffmpeg 和 QuickTime 讀得出來，所以很容易
    // 誤判成「Windows 的問題」）。成本是每 2 秒一個 seek + 8 bytes + flush。
    if (_frames - _hdrAt >= (uint32_t)(2.0f * TC_SAMPLE_RATE)) {
      _w.flushHeader();
      _hdrAt = _frames;
    }
  }

  // 單邊嚴重落後代表 SD 寫得太慢，倒掉多的那邊以免永久錯位
  while (_qL->available() > 6) { _qL->readBuffer(); _qL->freeBuffer(); _dropped++; }
  while (_qR->available() > 6) { _qR->readBuffer(); _qR->freeBuffer(); _dropped++; }
}

void StereoCapture::stop() {
  if (!_active) return;
  service();
  if (_fill) { _w.writeSamples(_buf, _fill); _fill = 0; }
  _qL->end();
  _qR->end();
  while (_qL->available()) { _qL->readBuffer(); _qL->freeBuffer(); }
  while (_qR->available()) { _qR->readBuffer(); _qR->freeBuffer(); }
  _w.close();
  _active = false;

  Serial.printf("[CAP] 完成：%.2f 秒，%lu KB%s\n",
                seconds(), (unsigned long)(_w.bytesWritten() / 1024),
                _dropped ? "  (有掉格，建議改用 Teensy 4.1 內建 SD 插槽)" : "");
}

// ============================================================================
bool Recorder::start(const char *path, uint32_t seconds) {
  if (_active || !_q) return false;

  _target  = (uint32_t)(TC_SAMPLE_RATE * seconds);
  // 事先把正確長度寫進標頭，就算之後補正失敗，檔案照樣讀得出來
  if (!_w.open(path, (uint32_t)TC_SAMPLE_RATE, 1, _target)) return false;
  _hdrAt = 0;

  _written = 0;
  _fill    = 0;
  _peak    = 0.0f;
  _active  = true;

  _q->begin();
  Serial.printf("[REC] 錄音開始 %lu 秒 -> %s\n", (unsigned long)seconds, path);
  return true;
}

// ============================================================================
//  連續採樣：等待 -> 觸發 -> 錄音 -> 回到等待
// ============================================================================
void Recorder::armSession(float threshold) {
  _session   = true;
  _thresh    = threshold;
  _preHead   = 0;
  _preCount  = 0;
  _gate.arm(threshold, millis());
  if (_q) {
    _q->begin();
    while (_q->available() > 0) { _q->readBuffer(); _q->freeBuffer(); }
  }
  Serial.printf("[SAMP] 連續採樣模式：偵測到聲音就自動錄 %d 秒。按任意鍵結束。\n",
                TC_REC_SECONDS);
  Serial.printf("       先聽 %d ms 量環境噪音，這段期間不會觸發…\n", TC_TRIG_CAL_MS);
}

void Recorder::endSession() {
  _session = false;
  if (_active) {                       // 錄到一半就中止
    if (_fill) { _w.writeSamples(_buf, _fill); _fill = 0; }
    _w.close();
    _active = false;
  }
  if (_q) {
    _q->end();
    while (_q->available() > 0) { _q->readBuffer(); _q->freeBuffer(); }
  }
  Serial.println(F("[SAMP] 採樣模式結束"));
}

// ---------------------------------------------------------------------------
//  純監看模式：不錄音、不寫 SD，只把輸入電平算出來。
//  麥克風接線／增益／輸入來源有沒有問題，看這個最快。
void Recorder::beginMonitor() {
  if (!_q) return;
  _monitor = true;
  _monPeak = 0.0f;
  _monRms  = 0.0f;
  _monDc   = 0.0f;
  _monAc   = 0.0f;
  _q->begin();
  while (_q->available() > 0) { _q->readBuffer(); _q->freeBuffer(); }
}

void Recorder::endMonitor() {
  if (!_q) return;
  _monitor = false;
  _q->end();
  while (_q->available() > 0) { _q->readBuffer(); _q->freeBuffer(); }
}

void Recorder::serviceMonitor() {
  if (!_monitor) return;
  while (_q->available() > 0) {
    int16_t *src = _q->readBuffer();
    // 一階低通的係數：a = exp(-2π·fc/fs)
    // 300 Hz -> 0.9578，2 kHz -> 0.7514（@44.1 kHz）
    const float A1 = 0.9578f, A2 = 0.7514f;

    float pk = 0.0f, sum = 0.0f, dc = 0.0f, sLo = 0.0f, sMid = 0.0f, sHi = 0.0f;
    for (int i = 0; i < TC_BLOCK; i++) {
      float a = src[i] * (1.0f / 32768.0f);
      float m = fabsf(a);
      if (m > pk) pk = m;
      sum += a * a;
      dc  += a;

      _lp1 = A1 * _lp1 + (1.0f - A1) * a;      // < 300 Hz
      _lp2 = A2 * _lp2 + (1.0f - A2) * a;      // < 2 kHz
      const float lo = _lp1;
      const float hi = a - _lp2;               // > 2 kHz
      const float mid = _lp2 - _lp1;           // 300 Hz ~ 2 kHz
      sLo += lo * lo; sMid += mid * mid; sHi += hi * hi;
    }
    _q->freeBuffer();
    if (pk > _monPeak) _monPeak = pk;                 // 保持峰值，按 o 重置
    // 直流 = 這個 block 的平均值；交流 RMS = 扣掉直流之後的標準差。
    // 一個固定的直流偏移會讓總 RMS 看起來「有訊號」，但它不是聲音。
    const float mean = dc / TC_BLOCK;
    const float var  = sum / TC_BLOCK - mean * mean;
    _monDc   = _monDc   * 0.8f + mean * 0.2f;
    _monAc   = _monAc   * 0.8f + sqrtf(var > 0.0f ? var : 0.0f) * 0.2f;
    _monRms  = _monRms  * 0.8f + sqrtf(sum  / TC_BLOCK) * 0.2f;
    _monLow  = _monLow  * 0.8f + sqrtf(sLo  / TC_BLOCK) * 0.2f;
    _monMid  = _monMid  * 0.8f + sqrtf(sMid / TC_BLOCK) * 0.2f;
    _monHigh = _monHigh * 0.8f + sqrtf(sHi  / TC_BLOCK) * 0.2f;
  }
}

// 等待狀態：一邊把資料丟進環形緩衝，一邊讓 TriggerGate 決定要不要開錄。
//
// 判斷邏輯全部在 trigger.cpp，這裡只負責搬資料 —— 那個狀態機在桌機上
// 有完整測試（tools/sim/trigger_test），不需要靠燒錄來驗。
void Recorder::serviceArmed() {
  const bool wasCal = _gate.calibrating();
  while (_q->available() > 0) {
    int16_t *src = _q->readBuffer();

    // 用第 4 大的取樣值而不是最大值 —— 單根數位脈衝不該被當成有人在彈。
    // 理由與量測值見 trigger.h 的 tcBlockLevel()。
    const float pk = tcBlockLevel(src, TC_BLOCK);

    // 寫進環形前置緩衝
    memcpy(_pre + (size_t)_preHead * TC_BLOCK, src, TC_BLOCK * 2);
    _preHead = (_preHead + 1) % TC_PREROLL_BLOCKS;
    if (_preCount < TC_PREROLL_BLOCKS) _preCount++;
    _q->freeBuffer();

    if (_gate.feed(pk, millis())) {
      startFromTrigger();
      return;
    }
  }
  if (wasCal && !_gate.calibrating()) {
    Serial.printf("[SAMP] 環境噪音 %.4f -> 實際門檻 %.4f", _gate.ambient(), _gate.threshold());
    if (_gate.threshold() > _thresh * 1.001f)
      Serial.printf("（比設定的 %.4f 高，是環境噪音撐上去的）", _thresh);
    Serial.println();
    if (!_gate.armedReady())
      Serial.println(F("       ** 環境噪音比你設的下限還大。門檻已自動抬高，"
                       "但訊噪餘裕會很小 —— 建議先安靜下來或提高音量 **"));
    if (_gate.calSpikes() > 0) {
      // 環境估計已經把它們剔除掉了（取第 8 大而不是最大值），所以門檻沒被
      // 毀掉；但孤立尖峰本身通常是數位耦合或接地問題，不該默默放過。
      Serial.printf("       校正期間有 %d 個孤立尖峰（遠高於環境噪音）。"
                    "門檻沒有被它們影響，\n"
                    "       但那通常是數位耦合／接地問題 —— 麥克風線離 SD 卡與"
                    "OLED 的排線遠一點會有幫助。\n", _gate.calSpikes());
    }
  }
}

// 觸發後：先把前置緩衝倒進檔案，再接著錄
bool Recorder::startFromTrigger() {
  _target = (uint32_t)(TC_SAMPLE_RATE * TC_REC_SECONDS);
  if (!_w.open(TC_REC_PATH, (uint32_t)TC_SAMPLE_RATE, 1, _target)) return false;
  _hdrAt = 0;

  _written = 0;
  _fill    = 0;
  _peak    = 0.0f;
  _active  = true;

  // 依環形順序倒出前置緩衝（最舊的先）
  int start = (_preHead - _preCount + TC_PREROLL_BLOCKS) % TC_PREROLL_BLOCKS;
  for (int b = 0; b < _preCount; b++) {
    int idx = (start + b) % TC_PREROLL_BLOCKS;
    _w.writeSamples(_pre + (size_t)idx * TC_BLOCK, TC_BLOCK);
    _written += TC_BLOCK;
  }
  _preCount = 0;

  Serial.printf("[SAMP] 觸發！（含 %d ms 前置緩衝）\n",
                (int)(TC_PREROLL_BLOCKS * TC_BLOCK_SEC * 1000));
  return true;
}

bool Recorder::service() {
  if (_session && !_active) { serviceArmed(); return false; }
  if (!_active) return false;

  while (_q->available() > 0) {
    int16_t *src = _q->readBuffer();
    memcpy(_buf + _fill, src, TC_BLOCK * 2);
    _q->freeBuffer();

    // 順便追蹤峰值，錄完可以提示使用者音量夠不夠
    for (int i = 0; i < TC_BLOCK; i++) {
      float a = fabsf(_buf[_fill + i] * (1.0f / 32768.0f));
      if (a > _peak) _peak = a;
    }

    _fill += TC_BLOCK;
    if (_fill >= 256) {
      _w.writeSamples(_buf, 256);
      _fill = 0;
    }
    _written += TC_BLOCK;
    // 同 StereoCapture：錄到一半斷電也要留下一個播得出來的檔案
    if (_written - _hdrAt >= (uint32_t)(2.0f * TC_SAMPLE_RATE)) {
      _w.flushHeader();
      _hdrAt = _written;
    }

    if (_written >= _target) break;
  }

  if (_written >= _target) {
    if (_fill) { _w.writeSamples(_buf, _fill); _fill = 0; }
    if (!_session) {                       // 單次錄音才關佇列；採樣模式要繼續聽
      _q->end();
      while (_q->available() > 0) { _q->readBuffer(); _q->freeBuffer(); }
    }
    _w.close();
    _active = false;
    _preCount  = 0;
    _gate.noteRecorded(millis());
    Serial.printf("[REC] 完成，%lu 取樣，峰值 %.2f%s\n",
                  (unsigned long)_written, _peak,
                  _peak < 0.05f ? "  <-- 太小聲，請調高 micGain 或靠近麥克風"
                                : (_peak > 0.98f ? "  <-- 削波了，請調低 micGain" : ""));
    return true;
  }
  return false;
}

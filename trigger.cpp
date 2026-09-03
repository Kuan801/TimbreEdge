#include "trigger.h"

#include <math.h>

float tcBlockLevel(const int16_t *src, int n) {
  if (!src || n <= 0) return 0.0f;
  // 維持前 K 大（遞減）。K 只有 4，直接插入排序，每個取樣最多比 4 次。
  float top[TC_BLOCK_TOPK];
  for (int i = 0; i < TC_BLOCK_TOPK; i++) top[i] = 0.0f;
  for (int i = 0; i < n; i++) {
    const int16_t v = src[i];
    const float a = (v < 0 ? -(float)v : (float)v) * (1.0f / 32768.0f);
    for (int k = 0; k < TC_BLOCK_TOPK; k++) {
      if (a > top[k]) {
        for (int j = TC_BLOCK_TOPK - 1; j > k; j--) top[j] = top[j - 1];
        top[k] = a;
        break;
      }
    }
  }
  // 取樣數少於 K 時沒有東西可以剔除，退回最大值
  return (n >= TC_BLOCK_TOPK) ? top[TC_BLOCK_TOPK - 1] : top[0];
}

void TriggerGate::recomputeThreshold() {
  const float fromAmbient = _ambient * TC_TRIG_MARGIN;
  _thresh = (fromAmbient > _base) ? fromAmbient : _base;
}

// 「安靜」= 回到環境噪音的水準，不是「門檻的某個比例」。
// 理由寫在 config.h 的 TC_TRIG_QUIET_MARGIN —— 用門檻當基準會讓環境噪音
// 自己就跨過安靜線，於是永遠武裝不起來。
float TriggerGate::quietLevel() const {
  const float fromAmbient = _ambient * TC_TRIG_QUIET_MARGIN;
  const float fromBase    = _base * TC_TRIG_QUIET_FRAC;
  return (fromAmbient > fromBase) ? fromAmbient : fromBase;
}

void TriggerGate::arm(float baseThreshold, uint32_t nowMs) {
  _base        = (baseThreshold > 0.0f) ? baseThreshold : TC_TRIG_LEVEL;
  _ambient     = 0.0f;
  _thresh      = _base;
  _level       = 0.0f;
  _lastTrigPk  = 0.0f;
  _lastAmb     = 0.0f;
  _calibrating = true;
  _calN        = 0;
  _calSpikes   = 0;
  for (int i = 0; i < CAL_KEEP; i++) _calTop[i] = 0.0f;
  _rearmed     = false;
  _quiet       = false;
  _calUntil    = nowMs + TC_TRIG_CAL_MS;
  _quietSince  = nowMs;
  _hot         = 0;
}

void TriggerGate::noteRecorded(uint32_t nowMs) {
  // 錄完的當下，音還在響。清掉武裝並重新開始計算安靜時間，
  // 尾巴就不會被當成下一個音。
  _rearmed    = false;
  _quiet      = false;
  _hot        = 0;
  _quietSince = nowMs;
}

bool TriggerGate::feed(float blockPeak, uint32_t nowMs) {
  _level = _level * 0.7f + blockPeak * 0.3f;

  // ---- 開頭先聽一段，量這個房間有多吵 ------------------------------------
  //
  // 用高百分位而不是平均：會誤觸發的是尖峰，不是平均值。但也不能用最大值
  // —— 一個孤立的數位尖峰就會把環境估到天上去，之後永遠觸發不了。
  // 保留最大的 CAL_KEEP 個，最後取其中最小的那個（≈96 百分位）。
  // 詳細理由見 trigger.h 的 CAL_KEEP 說明。
  if (_calibrating) {
    // 插入排序維持遞減。CAL_KEEP 只有 8，每個 block 最多比 8 次。
    for (int i = 0; i < CAL_KEEP; i++) {
      if (blockPeak > _calTop[i]) {
        for (int j = CAL_KEEP - 1; j > i; j--) _calTop[j] = _calTop[j - 1];
        _calTop[i] = blockPeak;
        break;
      }
    }
    if (_calN < 1000000) _calN++;
    if ((int32_t)(nowMs - _calUntil) >= 0) {
      _calibrating = false;
      // block 數少於 CAL_KEEP 時沒有足夠樣本可以剔除離群值（600 ms 正常有
      // 約 200 個，只有測試會走到這裡），那就退回「手上最小的那個」。
      const int idx = (_calN >= CAL_KEEP) ? (CAL_KEEP - 1)
                                          : (_calN > 0 ? _calN - 1 : 0);
      _ambient = _calTop[idx];
      // 被剔除掉的那幾個裡面，有多少是「遠高於環境」的真尖峰。
      // 門檻已經不受它們影響，但它們本身是硬體徵兆，要報出來。
      _calSpikes = 0;
      for (int i = 0; i < idx; i++)
        if (_calTop[i] > _ambient * 4.0f) _calSpikes++;
      recomputeThreshold();
      // 剛剛那 600 ms 就是一段觀察過的安靜，可以省掉再等 400 ms。
      //
      // 判準是「環境低於使用者設的下限」。不能拿 _thresh 來比 ——
      // _thresh 本來就是 _ambient × 1.5，那個比較永遠成立，等於沒判。
      // （這一條是桌機測試抓到的：校正期間餵滿格訊號，它照樣說已武裝。）
      //
      // 環境比使用者設的下限還吵，代表這個房間的噪音已經到訊號等級了。
      // 那就走正常流程：等一段真正的安靜再說，順便讓使用者看到門檻被抬高。
      _rearmed    = (_ambient < _base);
      _quiet      = _rearmed;
      _quietSince = nowMs;
    }
    return false;
  }

  // ---- 安靜追蹤（武裝的唯一條件）------------------------------------------
  if (blockPeak < quietLevel()) {
    if (!_quiet) { _quiet = true; _quietSince = nowMs; }
    if ((int32_t)(nowMs - _quietSince) >= (int32_t)TC_REARM_SILENT_MS) {
      _rearmed = true;
      // 安靜的時候才讓環境估計值跟著走。係數很慢（每個 block 0.5%，
      // 2.9 ms 一格 → 時間常數約 0.6 秒），這樣風扇轉起來門檻會跟著抬，
      // 但一個音的起音不足以把它拉高 —— 何況這一段只在「低於門檻」時執行。
      _ambient = _ambient * 0.995f + blockPeak * 0.005f;
      recomputeThreshold();
    }
  } else {
    _quiet = false;
  }

  // ---- 觸發 ---------------------------------------------------------------
  if (blockPeak >= _thresh) {
    _hot++;
    if (_hot >= TC_TRIG_BLOCKS && _rearmed) {
      _lastTrigPk = blockPeak;
      _lastAmb    = _ambient;
      _rearmed    = false;
      _hot        = 0;
      return true;
    }
  } else {
    _hot = 0;
  }
  return false;
}

float TriggerGate::lastHeadroomDb() const {
  if (_lastTrigPk <= 0.0f) return 0.0f;
  // 環境量到 0 是有可能的（完全安靜的房間 + 沒接麥克風）。
  // 那種情況下餘裕無限大，回一個大但有限的數字，不要回 inf。
  if (_lastAmb <= 1e-5f) return 99.0f;
  return 20.0f * log10f(_lastTrigPk / _lastAmb);
}

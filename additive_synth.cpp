#include "additive_synth.h"

// ----------------------------------------------------------- 正弦查表 ------
// 1024 點 + 1 個 wrap 守衛，線性內插後 THD 約 -78 dB，對 64 諧波加法仍足夠。
static float  sSine[TC_SINE_TBL_SIZE + 1];
static bool   sSineReady = false;

static void buildSine() {
  if (sSineReady) return;
  for (int i = 0; i <= TC_SINE_TBL_SIZE; i++)
    sSine[i] = sinf(2.0f * (float)M_PI * i / TC_SINE_TBL_SIZE);
  sSineReady = true;
}

static inline float sineLookup(uint32_t phase) {
  uint32_t idx  = phase >> (32 - TC_SINE_TBL_BITS);          // 0..1023
  float    frac = (float)(phase & ((1u << (32 - TC_SINE_TBL_BITS)) - 1))
                  * (1.0f / (float)(1u << (32 - TC_SINE_TBL_BITS)));
  float a = sSine[idx], b = sSine[idx + 1];
  return a + (b - a) * frac;
}

// ---------------------------------------------------------- 軟限幅 --------
// 0 ~ 0.75 FS 完全線性（不碰動態），只有超過門檻的尖峰才用 tanh 壓進 1.0 FS。
// 早期版本用 x/(1+|x|/k)，那會把整段訊號都壓縮掉，量測時發現持續段被抬高、
// 起音被壓扁，等於整體動態少了一半 —— 這裡改成有膝點的版本。
#define SC_THRESH 24575.0f            // 0.75 * 32767
#define SC_RANGE  8192.0f             // 32767 - SC_THRESH
static inline float softClip(float x) {
  float a = fabsf(x);
  if (a <= SC_THRESH) return x;
  float t = (a - SC_THRESH) / SC_RANGE;
  float y = SC_THRESH + SC_RANGE * tanhf(t);
  return (x < 0.0f) ? -y : y;
}

static inline uint32_t hzToInc(float hz) {
  if (hz <= 0.0f) return 0;
  return (uint32_t)(hz * (4294967296.0f / TC_SAMPLE_RATE));
}

// 每個聲部一份暫存 buffer（audio ISR 內用，不要放堆疊）
DMAMEM static float sVoiceBuf[TC_BLOCK];
DMAMEM static float sAccL[TC_BLOCK];
DMAMEM static float sAccR[TC_BLOCK];

// ---------------------------------------------------------------------------
AudioSynthAdditive::AudioSynthAdditive() : AudioStream(0, NULL) {
  buildSine();
  for (int i = 0; i < TC_N_VOICES; i++) {
    _v[i].rng = 0x13579BDFu ^ (uint32_t)(i * 2654435761u);
    for (int h = 0; h < TC_N_PARTIAL; h++) {
      _v[i].phase[h]     = 0;
      _v[i].amp[h]       = 0.0f;
      _v[i].ampStep[h]   = 0.0f;
      _v[i].baseInc[h]   = 0;
      _v[i].onsetT[h]    = 0.0f;
      _v[i].shimPhase[h] = 0.0f;
      _v[i].shimInc[h]   = 0.0f;
    }
  }
}

// ---------------------------------------------------------------------------
int AudioSynthAdditive::allocVoice(float midi) {
  // 1) 同音高已在發聲 -> 直接重觸發
  for (int i = 0; i < TC_N_VOICES; i++)
    if (_v[i].stage != IDLE && fabsf(_v[i].midi - midi) < 0.01f) return i;
  // 2) 空的
  for (int i = 0; i < TC_N_VOICES; i++)
    if (_v[i].stage == IDLE) return i;
  // 3) 偷最舊的 release 中聲部，再不然偷最舊的
  int best = -1;
  uint32_t oldest = 0xFFFFFFFFu;
  for (int i = 0; i < TC_N_VOICES; i++)
    if (_v[i].stage == RELEASE && _v[i].age < oldest) { oldest = _v[i].age; best = i; }
  if (best >= 0) return best;
  oldest = 0xFFFFFFFFu;
  for (int i = 0; i < TC_N_VOICES; i++)
    if (_v[i].age < oldest) { oldest = _v[i].age; best = i; }
  return best;
}

// ---------------------------------------------------------------------------
AudioSynthAdditive::NoteResult
AudioSynthAdditive::noteOn(float midi, float vel, float pan) {
  if (!_model) return NOTE_NO_MODEL;
  // 每個音符各自挑「音高最接近的取樣點」，移調距離縮到半音以內
  const InstrumentProfile *p = _model->profileFor(tc_midiToHz(midi));
  if (!p || !p->valid) return NOTE_NO_TIMBRE;

  int i = allocVoice(midi);
  if (i < 0) return NOTE_NO_VOICE;
  Voice &v = _v[i];

  v.midi  = midi;
  v.f0    = tc_midiToHz(midi);
  v.vel   = tc_clampf(vel, 0.05f, 1.0f);
  v.pan   = tc_clampf(pan, 0.0f, 1.0f);
  v.tSec  = 0.0f;
  v.stage = PLAYING;
  // 尾段狀態一定要清掉。同一個聲部被重複使用時，不清會延續上一個音的尾段高度，
  // 新的音一開始就從很小的振幅接上去。
  v.envTail = 0.0f;
  v.age   = _ageCounter++;
  v.vibPhase = 0.0f;

  // --- 包絡：直接播 profile 量到的曲線 -------------------------------------
  // 不再用 A/D/S/R 四個參數去逼近。鋼琴的雙段衰減、提琴的漸強，
  // 參數化模型都表達不出來，但量到的曲線本身就是對的。
  const float bs = TC_BLOCK_SEC;
  v.prof     = p;
  v.refDur   = fmaxf(p->noteDur, 0.2f);
  v.holdNorm = (p->envHoldNorm > 0.01f) ? p->envHoldNorm : 1.0f;
  v.rCoef    = expf(-bs / fmaxf(p->release * 0.4f, 0.02f));

  // 包絡曲線走完之後怎麼辦。
  //
  // 衰減型：照量到的衰減率繼續往下掉。sustainDecayPerSec 是「每秒的振幅倍率」，
  //         換算成每個 block 就是開 (bs/1秒) 次方。
  // 持續型：維持不變（運弓/吹氣還在繼續），也就是 1.0。
  //
  // 這一項以前不存在，音會停在 loud[31] 的高度不動。實測鋼琴的 loud[] 只掉到
  // −24 dB，於是每個音都在 −24 dB 平住，卡農聽起來像管風琴。
  // sustainDecayPerSec 的註解一直寫著「長長的自然衰減由它負責」，
  // 但它從來沒被合成器讀過 —— 這裡才真的接上。
  {
    const float perSec = p->sustainDecayPerSec;
    v.tailCoef = (perSec > 0.0f && perSec < 0.999f) ? powf(perSec, bs) : 1.0f;
  }

  // 依音高決定要發幾根諧波：塞滿到 Nyquist，低音才不會悶
  v.nPart = tc_partialCount(v.f0);

  // 諧波頻率（含非諧性）與噪聲濾波係數
  for (int h = 0; h < v.nPart; h++)
    v.baseInc[h] = hzToInc(_model->harmonicHz(p, v.f0, h));

  // 噪聲層做成「帶通」而不是低通。
  //
  // 舊版把噪聲低通在 5*f0（長笛 A4 只有 2.2 kHz），可是真實的氣聲/弓噪是
  // 寬頻的。同樣的總能量擠進 1/9 的頻寬，每個頻格就高出 10*log10(9)=9.6 dB，
  // 實測長笛諧波間底噪因此比真值高 11 dB —— 能量對了、頻寬錯了。
  //
  //   下緣 fLo = 5*f0：低於這裡的殘差由諧波抖動負責（邊帶貼在諧波上，
  //                    位置才對），寬頻層只接手 5*f0 以上的那一份。
  //   上緣 fHi = 樂器自己的頻譜包絡掉到峰值 -TC_NOISE_ROLL_DB 的頻率。
  //
  // 上緣為什麼不能是 8*f0（這是音頭沙沙聲的成因）：8*f0 只看音高，不看
  // 這件樂器在那個頻段還有沒有能量。鋼琴 F#5 的噪聲層會被放到 3.7~5.9 kHz，
  // 而鋼琴的頻譜包絡在 2.5 kHz 就 -40 dB、5 kHz 是 -67 dB —— in-situ 量到
  // 起音的 5 kHz 以上，噪聲層比鋼琴自己的諧波大 15~23 dB。沒有東西遮蔽它。
  // 改用 specEnv 之後，同一個量測從「多 15.2 dB」變成「少 5.1 dB」。
  // specEnv 本來就存在 profile 裡，所以這個改動不需要重跑分析。
#ifdef TC_NOISE_FIXED_BAND
  // 實驗：帶通改成固定的絕對頻帶（不跟音高走），下限仍不低於 f0
  float fLo = tc_clampf(fmaxf((float)TC_NOISE_FLO, v.f0), 150.0f, 6000.0f);
  float fHi = tc_clampf((float)TC_NOISE_FHI, fLo * 1.5f, 9000.0f);
#else
  float fRoll = TC_SPECENV_FMAX;
  {
    float pk = -1e30f;
    for (int q = 0; q < TC_SPECENV_PTS; q++) if (p->specEnv[q] > pk) pk = p->specEnv[q];
    const float th = pk - TC_NOISE_ROLL_DB;
    for (int q = TC_SPECENV_PTS - 1; q >= 0; q--) {
      if (p->specEnv[q] > th) {
        fRoll = TC_SPECENV_FMIN * powf(TC_SPECENV_FMAX / TC_SPECENV_FMIN,
                                       (float)q / (float)(TC_SPECENV_PTS - 1));
        break;
      }
    }
  }
  float fHi = tc_clampf(fRoll, 900.0f, 8000.0f);
  // 下緣不能超過上緣的一半。高音區的 5*f0 常常已經在樂器的頻譜之外，
  // 硬撐會把帶通壓成一個幾乎沒有通帶的空殼，底下的 RMS 正規化再把剩下
  // 那一點點放大好幾倍 —— 那是上一版「每級轉角上限 3 kHz」的下場。
  float fLo = tc_clampf(fminf(v.f0 * 5.0f, fHi * 0.5f), 200.0f, 4000.0f);
#endif
  v.noiseFLo = fLo;
  v.noiseFHi = fHi;

  // 帶通用 biquad（RBJ，Q = 1/sqrt(2)）：低通串 TC_NOISE_LP_STAGES 級、
  // 高通一級。一階遞迴 y += c*(x-y) 在 c -> 1 時退化成 y = x，高音區等於
  // 完全沒有低通；雙線性轉換的 biquad 沒有這個問題，而且 12 dB/oct 一級的
  // 滾降才擋得住 5 kHz 以上的漏出。
  {
    const float Q = 0.70710678f;
    {
      const float w = 2.0f * (float)M_PI * fHi / TC_SAMPLE_RATE;
      const float cw = cosf(w), al = sinf(w) / (2.0f * Q), a0 = 1.0f + al;
      v.nbLpB[0] = (1.0f - cw) * 0.5f / a0;
      v.nbLpB[1] = (1.0f - cw) / a0;
      v.nbLpB[2] = v.nbLpB[0];
      v.nbLpA[0] = (-2.0f * cw) / a0;
      v.nbLpA[1] = (1.0f - al) / a0;
    }
    {
      const float w = 2.0f * (float)M_PI * fLo / TC_SAMPLE_RATE;
      const float cw = cosf(w), al = sinf(w) / (2.0f * Q), a0 = 1.0f + al;
      v.nbHpB[0] = (1.0f + cw) * 0.5f / a0;
      v.nbHpB[1] = -(1.0f + cw) / a0;
      v.nbHpB[2] = v.nbHpB[0];
      v.nbHpA[0] = (-2.0f * cw) / a0;
      v.nbHpA[1] = (1.0f - al) / a0;
    }
  }
  for (int k = 0; k < TC_NOISE_LP_STAGES; k++) { v.nbLpZ[k][0] = 0.0f; v.nbLpZ[k][1] = 0.0f; }
  v.nbHpZ[0] = v.nbHpZ[1] = 0.0f;
  // 濾波器的實際 RMS 增益用解析式推很容易出錯，直接跑 1024 個取樣量出來
  // —— 每個音符只做一次，成本可以忽略。
  {
    uint32_t r = v.rng ^ 0x5A5A5A5Au;
    float lz[TC_NOISE_LP_STAGES][2] = {{0.0f, 0.0f}};
    float hz[2] = {0.0f, 0.0f};
    float acc = 0.0f;
    for (int i = 0; i < 1024; i++) {
      r = r * 1664525u + 1013904223u;
      float x = ((int32_t)(r >> 8) * (1.0f / 8388608.0f)) - 1.0f;
      for (int k = 0; k < TC_NOISE_LP_STAGES; k++) {
        const float y = v.nbLpB[0] * x + lz[k][0];
        lz[k][0] = v.nbLpB[1] * x - v.nbLpA[0] * y + lz[k][1];
        lz[k][1] = v.nbLpB[2] * x - v.nbLpA[1] * y;
        x = y;
      }
      {
        const float y = v.nbHpB[0] * x + hz[0];
        hz[0] = v.nbHpB[1] * x - v.nbHpA[0] * y + hz[1];
        hz[1] = v.nbHpB[2] * x - v.nbHpA[1] * y;
        x = y;
      }
      if (i >= 256) acc += x * x;          // 前 256 個當暖機，不計入
    }
    v.noiseNrm = 1.0f / (sqrtf(acc / 768.0f) + 1e-6f);
  }
  // 起音的寬頻層與持續段共用同一組帶通：兩者的「落點」都已經由量測決定，
  // 不需要再為起音另外寫死一個 9 kHz 的低通。
  // attackNoise / noiseGain 都是「能量比」，要開根號轉成振幅比才能直接當倍率用
  // （同 timbre_model.cpp 的說明）。
  // 起音多出來的那份非週期能量，也照量到的落點拆成「高頻寬頻」與「貼在諧波上」
  // 兩份，跟持續段用同一套邏輯 —— 不必為不同樂器寫不同分支。
  {
    float extra = tc_clampf(p->attackNoise - p->noiseGain, 0.0f, 0.9f);   // 能量比
#ifdef TC_DBG_NO_ATK
    extra = 0.0f;                    // 除錯：整個起音額外能量都不要
#endif
    float hi    = tc_clampf(p->attackHighFrac, 0.0f, 0.9f);
    v.noiseAtk  = sqrtf(extra * hi);              // 寬頻層（振幅）
    v.atkJitVar = extra * (1.0f - hi);            // 抖動層（變異數，comp 稍後再乘）
  }

  // 持續段的非週期能量拆成兩份（見 update() 的 2b' 說明）：
  //   量到落在低頻的那份交給諧波抖動 —— 邊帶貼在諧波上，位置才對
  //   剩下的才用寬頻噪聲層 —— 真實樂器確實有一小部分寬頻氣流聲
  {
    float nf = tc_clampf(p->noiseGain, 0.0f, 0.9f);
    // 要讓「合成出來、用週期差分量到的非週期比」等於分析器量到的 noiseGain，
    // 得把兩層衰減補回去：
    //
    // (1) 週期差分 x[n]-x[n-T] 對邊帶的增益是 2|sin(pi*df/f0)| —— 愈貼近載波
    //     愈量不到。抖動的邊帶散佈在 0~B（B = block 率的一半 = 172 Hz），
    //     取平均得 mean(sin^2) = 0.5 - sin(2*pi*B/f0)/(4*pi*B/f0)，
    //     量到的比例 = 2 * sigma^2 * mean(sin^2)。這一項跟音高有關：
    //     長笛 A4 要補 1.35 倍，鋼琴 C4 只要 0.83 倍，用固定常數會兩頭不討好。
    // (2) block 內的線性內插又多做一次低通，實測再差 1.1 倍。
    const float blkNyq = 0.5f / TC_BLOCK_SEC;          // 172 Hz
    const float xb = 2.0f * (float)M_PI * blkNyq / v.f0;
    float meanSin2 = 0.5f - ((fabsf(xb) > 1e-4f) ? sinf(xb) / (2.0f * xb) : 0.5f);
    meanSin2 = tc_clampf(meanSin2, 0.05f, 0.5f);
    const float comp = 1.0f / (2.0f * meanSin2 * 0.83f);   // 0.83 = 內插造成的額外衰減
    // 諧波抖動 vs 寬頻噪聲的分配比例，由量測到的殘差落點決定，不是固定值：
    // 貼在諧波上的低頻殘差 -> 抖動；5*f0 以上的殘差 -> 寬頻層。
    // 實測長笛 15%、提琴 60% 走寬頻，這正好對應氣聲與弓噪的差別。
    float hiFrac = tc_clampf(p->noiseHighFrac, 0.0f, 0.9f);
    v.jitFrac = 1.0f - hiFrac;
    v.jitterSigma = sqrtf(nf * v.jitFrac * comp * TC_JITTER_CAL);   // 見 config.h 的說明

    // 抖動深度的上限，以及「溢出的部分改走寬頻層」。
    //
    // sigma 超過 1/sqrt(3) 之後，乘數 1 + sigma*jit 會撞到 clamp 的兩端，
    // 抖動退化成每 2.9 ms 一次的隨機開關（實測 B5 有 68% 的抽樣撞牆）——
    // 那就是音頭沙沙聲的來源。詳見 config.h 的 TC_JIT_SIGMA_MAX。
    //
    // 溢出的變異數要搬到寬頻噪聲層，不能丟掉：兩者都是在描述同一份「起音的
    // 非週期能量」，只是位置不同。丟掉的話起音會變得比真實素材還乾淨 ——
    // 實測合成的起音非週期比本來就只有參考的 0.2 倍，再少就更不像了。
    {
      const float sigMax2 = TC_JIT_SIGMA_MAX * TC_JIT_SIGMA_MAX;
      if (v.atkJitVar * comp > sigMax2) {
        const float keep  = sigMax2 / comp;             // 抖動層留得住的變異數（能量比）
        const float spill = v.atkJitVar - keep;
        v.atkJitVar = keep;
        // 兩者都是能量比，平方相加才對（noiseAtk 存的是振幅）
        v.noiseAtk  = sqrtf(v.noiseAtk * v.noiseAtk + spill);
      }
      v.atkJitSigma = sqrtf(v.atkJitVar * comp);
      // 持續段實測只有 0.02~0.15，離上限很遠，但別留一條會炸的路徑
      if (v.jitterSigma > TC_JIT_SIGMA_MAX) v.jitterSigma = TC_JIT_SIGMA_MAX;
    }
    for (int h = 0; h < TC_N_PARTIAL; h++) v.jit[h] = 0.0f;
  }

  // 顫音深度由素材決定：提琴有、鋼琴沒有。統一加顫音會讓所有樂器同一個味道。
  v.vibCents = tc_clampf(p->vibratoCents, 0.0f, _vibMaxCents);
  // 顫音速率也用量到的，不再一律 4.8 Hz —— 不同樂器/演奏者差異很大
  v.vibHz    = (p->vibratoHz > 2.0f) ? p->vibratoHz : _vibHz;

  // 非同步起音：把量測到的逐諧波延遲搬過來。
  // 超過 TC_N_HARM 的部分沿用最後一根的延遲並隨諧波序號微幅外推。
  for (int h = 0; h < v.nPart; h++) {
    if (h < TC_N_HARM) v.onsetT[h] = p->harmOnset[h];
    else               v.onsetT[h] = p->harmOnset[TC_N_HARM - 1];
  }

  // shimmer：每根諧波一個獨立的慢速 LFO，頻率打散才不會聽成整齊的顫音。
  // profile 存的是「標準差」，而正弦的標準差是振幅的 1/sqrt(2)，
  // 所以要乘 sqrt(2) 才能讓合成出來的起伏量等於量測值。
  v.shimDepth = tc_clampf(p->shimmerDepth * 1.41421356f, 0.0f, 0.30f);
  for (int h = 0; h < v.nPart; h++) {
    uint32_t r = (v.rng ^ (uint32_t)(h * 2654435761u)) * 1664525u + 1013904223u;
    float u1 = (float)(r >> 8) * (1.0f / 16777216.0f);
    r = r * 1664525u + 1013904223u;
    float u2 = (float)(r >> 8) * (1.0f / 16777216.0f);
    float hz = TC_SHIMMER_HZ_MIN + u1 * (TC_SHIMMER_HZ_MAX - TC_SHIMMER_HZ_MIN);
    v.shimInc[h]   = 2.0f * (float)M_PI * hz * TC_BLOCK_SEC;
    v.shimPhase[h] = u2 * 6.2831853f;
  }

  // 相位：起音瞬間讓所有諧波同相會產生「啪」的脈衝，稍微打散
  for (int h = 0; h < v.nPart; h++)
    v.phase[h] = (uint32_t)((h * 2654435761u) ^ (v.age * 40503u));

  if (v.env < 0.001f) {
    for (int h = 0; h < TC_N_PARTIAL; h++) { v.amp[h] = 0.0f; v.ampStep[h] = 0.0f; }
    v.noise = 0.0f; v.noiseStep = 0.0f;
  }
  return NOTE_OK;
}

// ---------------------------------------------------------------------------
void AudioSynthAdditive::noteOff(float midi) {
  for (int i = 0; i < TC_N_VOICES; i++)
    if (_v[i].stage != IDLE && _v[i].stage != RELEASE && fabsf(_v[i].midi - midi) < 0.01f)
      _v[i].stage = RELEASE;
}

void AudioSynthAdditive::allNotesOff() {
  for (int i = 0; i < TC_N_VOICES; i++)
    if (_v[i].stage != IDLE) _v[i].stage = RELEASE;
}

int AudioSynthAdditive::activeVoices() const {
  int n = 0;
  for (int i = 0; i < TC_N_VOICES; i++) if (_v[i].stage != IDLE) n++;
  return n;
}

// ---------------------------------------------------------------------------
void AudioSynthAdditive::renderVoice(Voice &v, float *dst) {
  for (int i = 0; i < TC_BLOCK; i++) dst[i] = 0.0f;

  // 顫音：起音後 0.25 秒才慢慢進來，模仿真人演奏。深度來自 profile。
  // 調變輪的份量是「額外加上去」的，而且不等 0.25 秒 —— 演奏者按下去就要有反應。
  float vibDepth = 0.0f;
  if (v.vibCents > 0.5f && v.tSec > 0.25f)
    vibDepth = tc_clampf((v.tSec - 0.25f) / 0.5f, 0.0f, 1.0f) * v.vibCents;
  vibDepth += _modCents;

  float vibMul = 1.0f;
  if (vibDepth > 0.0f) {
    v.vibPhase += 2.0f * (float)M_PI * v.vibHz * TC_BLOCK_SEC;
    if (v.vibPhase > 2.0f * (float)M_PI) v.vibPhase -= 2.0f * (float)M_PI;
    vibMul = powf(2.0f, (vibDepth * sinf(v.vibPhase)) / 1200.0f);
  }

  // ---- 諧波 --------------------------------------------------------------
  for (int h = 0; h < v.nPart; h++) {
    float a  = v.amp[h];
    float st = v.ampStep[h];
    if (a < 1e-6f && st <= 0.0f) { v.amp[h] = 0.0f; continue; }

    uint32_t ph  = v.phase[h];
    uint32_t inc = v.baseInc[h];
    float    mul = (vibDepth > 0.0f) ? vibMul * _bendMul : _bendMul;
    if (mul != 1.0f) inc = (uint32_t)(inc * mul);

    for (int i = 0; i < TC_BLOCK; i++) {
      dst[i] += sineLookup(ph) * a;
      ph     += inc;
      a      += st;
    }
    v.phase[h] = ph;
    v.amp[h]   = (a < 0.0f) ? 0.0f : a;
  }

  // ---- 噪聲層（氣聲 / 弓噪 / 擊弦雜音）------------------------------------
#ifdef TC_DBG_NO_BB
  if (false) {                       // 除錯：完全關掉寬頻噪聲層
#else
  if (v.noise > 1e-6f || v.noiseStep > 0.0f) {
#endif
    float n  = v.noise;
    float st = v.noiseStep;
    uint32_t r = v.rng;
    const float nrm = v.noiseNrm;
    const float lb0 = v.nbLpB[0], lb1 = v.nbLpB[1], lb2 = v.nbLpB[2];
    const float la0 = v.nbLpA[0], la1 = v.nbLpA[1];
    const float hb0 = v.nbHpB[0], hb1 = v.nbHpB[1], hb2 = v.nbHpB[2];
    const float ha0 = v.nbHpA[0], ha1 = v.nbHpA[1];
    float lz[TC_NOISE_LP_STAGES][2];
    for (int k = 0; k < TC_NOISE_LP_STAGES; k++) { lz[k][0] = v.nbLpZ[k][0]; lz[k][1] = v.nbLpZ[k][1]; }
    float hz0 = v.nbHpZ[0], hz1 = v.nbHpZ[1];
    for (int i = 0; i < TC_BLOCK; i++) {
      r = r * 1664525u + 1013904223u;
      float x = ((int32_t)(r >> 8) * (1.0f / 8388608.0f)) - 1.0f;   // -1..1
      // 轉置直接第二型：狀態只有兩個，係數固定，編譯器展得開
      for (int k = 0; k < TC_NOISE_LP_STAGES; k++) {
        const float y = lb0 * x + lz[k][0];
        lz[k][0] = lb1 * x - la0 * y + lz[k][1];
        lz[k][1] = lb2 * x - la1 * y;
        x = y;
      }
      {
        const float y = hb0 * x + hz0;
        hz0 = hb1 * x - ha0 * y + hz1;
        hz1 = hb2 * x - ha1 * y;
        x = y;
      }
      dst[i] += x * n * nrm * TC_NOISE_BB_GAIN;
      n  += st;
    }
    v.rng = r;
    for (int k = 0; k < TC_NOISE_LP_STAGES; k++) { v.nbLpZ[k][0] = lz[k][0]; v.nbLpZ[k][1] = lz[k][1]; }
    v.nbHpZ[0] = hz0; v.nbHpZ[1] = hz1;
    v.noise   = (n < 0.0f) ? 0.0f : n;
  }
}

// ---------------------------------------------------------------------------
void AudioSynthAdditive::update(void) {
  audio_block_t *bl = allocate();
  if (!bl) return;
  audio_block_t *br = allocate();
  if (!br) { release(bl); return; }

  for (int i = 0; i < TC_BLOCK; i++) { sAccL[i] = 0.0f; sAccR[i] = 0.0f; }

  const bool haveModel = (_model && _model->ready());

  // ---- 全域 partial 預算 --------------------------------------------------
  // 尾音疊起來時總諧波數可能衝到 512 根，會直接爆掉音訊中斷的時間預算。
  // 超過就等比例縮減，讓聲音略悶但不中斷。
  int wanted = 0;
  for (int i = 0; i < TC_N_VOICES; i++)
    if (_v[i].stage != IDLE) wanted += _v[i].nPart;

  float partScale = 1.0f;
  if (wanted > TC_PARTIAL_BUDGET) partScale = (float)TC_PARTIAL_BUDGET / (float)wanted;

  for (int i = 0; i < TC_N_VOICES; i++) {
    Voice &v = _v[i];
    if (v.stage == IDLE || !haveModel || !v.prof) continue;

    // ---- 1) 包絡：直接讀 profile 量到的曲線 -------------------------------
    if (v.stage == PLAYING) {
      float tn = tc_timeWarp(v.tSec, v.refDur);
      if (tn > v.holdNorm) tn = v.holdNorm;      // 持續型不要演出「收弓」

      if (tn >= 1.0f) {
        // 量到的曲線已經走完。衰減型要照量到的衰減率繼續往下，
        // 不能停在 loud[31] 的高度 —— 鋼琴不會在 −24 dB 平住不動。
        //
        // 從曲線的最後一格接續，所以第一次進到這裡不會有跳階。
        if (v.envTail <= 0.0f) v.envTail = v.prof->loud[TC_N_KEYFRAME - 1];
        v.envTail *= v.tailCoef;
        v.env = v.envTail;
        // 掉到聽不見就收掉，不要留著空轉佔用聲部
        if (v.env < 0.0006f) { v.env = 0.0f; v.stage = IDLE; continue; }
      } else {
        float pos = tc_clampf(tn, 0.0f, 1.0f) * (TC_N_KEYFRAME - 1);
        int   k   = (int)pos;
        if (k > TC_N_KEYFRAME - 2) k = TC_N_KEYFRAME - 2;
        float fr  = pos - k;
        v.env = v.prof->loud[k] * (1.0f - fr) + v.prof->loud[k + 1] * fr;
        if (v.env < 0.0f) v.env = 0.0f;
      }
    } else {                                     // RELEASE
      v.env *= v.rCoef;
      if (v.env < 0.0006f) {
        v.env = 0.0f;
        v.stage = IDLE;
        for (int h = 0; h < TC_N_PARTIAL; h++) { v.amp[h] = 0.0f; v.ampStep[h] = 0.0f; }
        v.noise = 0.0f; v.noiseStep = 0.0f;
        continue;
      }
    }
    v.tSec += TC_BLOCK_SEC;

    // ---- 2) 問模型：這一瞬間的諧波長怎樣 ---------------------------------
    // 預算不足時縮減這個聲部的諧波數；被砍掉的那些要淡出，不能直接切斷
    int nUse = (partScale < 1.0f) ? (int)(v.nPart * partScale) : v.nPart;
    if (nUse < 8) nUse = 8;
    if (nUse > v.nPart) nUse = v.nPart;
    for (int h = nUse; h < v.nPart; h++) v.ampStep[h] = -v.amp[h] * (1.0f / TC_BLOCK);

    float target[TC_N_PARTIAL], targetNoise = 0.0f;
    const float loud = v.env * v.vel;
    _model->harmonics(v.prof, v.f0, loud, tc_timeWarp(v.tSec, v.refDur),
                      v.stage == RELEASE, target, &targetNoise, nUse);

    // ---- 2a) 非同步起音：每根諧波用自己的時刻進場 ------------------------
    // 真實樂器的高次諧波晚幾十毫秒才進來，全部同時起音會非常「電子」。
    // 只在起音階段作用，之後閘門恆為 1，不影響持續段。
    if (v.tSec < 0.35f) {
      for (int h = 0; h < nUse; h++) {
        float t0 = v.onsetT[h];
        if (t0 <= 0.0f) continue;
        // 從 t0 起用 8 ms 淡入，避免硬切造成喀聲
        float g = (v.tSec - t0) * 125.0f;
        target[h] *= tc_clampf(g, 0.0f, 1.0f);
      }
    }

    // ---- 2b) shimmer：每根諧波獨立的慢速擺動 -----------------------------
    // 量測顯示真實樂器每根諧波約有 8% 的微觀起伏，完全沒有的話長音像管風琴。
    if (v.shimDepth > 0.001f) {
      for (int h = 0; h < nUse; h++) {
        v.shimPhase[h] += v.shimInc[h];
        if (v.shimPhase[h] > 6.2831853f) v.shimPhase[h] -= 6.2831853f;
        target[h] *= 1.0f + v.shimDepth * sinf(v.shimPhase[h]);
      }
    }

    // ---- 2b') 諧波抖動：非週期能量的主要來源 -----------------------------
    //
    // 量測真實長笛 A4 的殘差（x[n]-x[n-T]）頻譜分佈：
    //     0-0.9k 29.5%   0.9-2k 48.5%   2-5k 17.8%   5k 以上 4.2%
    // 78% 的非週期能量緊貼著 f0/h2/h3 —— 它根本不是寬頻的氣聲，
    // 而是每根諧波自己在微幅抖動所產生的邊帶。做成一層寬頻嘶聲時
    // 合成端變成 5k 以上佔 36%，位置整個錯掉，聽起來就是「加了嘶聲」
    // 而不是「這件樂器本來的呼吸感」。
    //
    // 改成直接對每根諧波做隨機幅度調變：a_h -> a_h * (1 + sigma * g)，
    // g 是單位變異數的隨機序列。這樣邊帶能量自動正比於該諧波的振幅，
    // 分佈跟真實的一模一樣，而總非週期比例恰好等於 sigma^2 ——
    // 也就是分析器量到的那個數，兩邊定義完全對得起來。
    //
    // 每個 block 抽一次，調變頻寬 172 Hz，邊帶就會貼在諧波附近。
    float sigNow = v.jitterSigma;
#ifdef TC_DBG_NO_ATKJIT
    if (false) {                     // 除錯：只關起音抖動，保留起音寬頻
#else
    if (v.atkJitSigma > 0.001f && v.tSec < 0.15f) {
#endif
      float e = expf(-v.tSec / 0.03f);
      sigNow = sqrtf(sigNow * sigNow + v.atkJitSigma * v.atkJitSigma * e * e);
    }
#ifdef TC_DBG_NO_JIT
    sigNow = 0.0f;                   // 除錯：完全關掉諧波抖動
#endif
    if (sigNow > 0.001f) {
      uint32_t r = v.rng;
      for (int h = 0; h < nUse; h++) {
        r = r * 1664525u + 1013904223u;
        float u = ((int32_t)(r >> 8) * (1.0f / 8388608.0f)) - 1.0f;   // -1..1
        v.jit[h] = u * 1.732f;                            // *sqrt(3) -> 單位變異數
        target[h] *= tc_clampf(1.0f + sigNow * v.jit[h], 0.0f, 2.5f);
      }
      v.rng = r;
    }

    // ---- 2c) 起音噪聲爆點（弓噪 / 氣聲 / 擊弦雜音）------------------------
    if (v.noiseAtk > 0.0f && v.tSec < 0.15f)
      targetNoise += v.noiseAtk * loud * expf(-v.tSec / 0.03f);

    const float invBlk = 1.0f / (float)TC_BLOCK;
    for (int h = 0; h < nUse; h++) v.ampStep[h] = (target[h] - v.amp[h]) * invBlk;
    v.noiseStep = (targetNoise - v.noise) * invBlk;

    // ---- 3) 產生取樣並做等功率 pan ---------------------------------------
    renderVoice(v, sVoiceBuf);
    float gl = cosf(v.pan * (float)M_PI_2);
    float gr = sinf(v.pan * (float)M_PI_2);
    for (int k = 0; k < TC_BLOCK; k++) {
      sAccL[k] += sVoiceBuf[k] * gl;
      sAccR[k] += sVoiceBuf[k] * gr;
    }
  }

  // ---- 4) 主音量 + 軟限幅 + 轉 int16 --------------------------------------
  const float g = _gain * 32767.0f;
  for (int k = 0; k < TC_BLOCK; k++) {
    bl->data[k] = (int16_t)softClip(sAccL[k] * g);
    br->data[k] = (int16_t)softClip(sAccR[k] * g);
  }

  transmit(bl, 0);
  transmit(br, 1);
  release(bl);
  release(br);
}

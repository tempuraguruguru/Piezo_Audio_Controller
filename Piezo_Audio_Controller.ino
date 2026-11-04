#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <SerialFlash.h>
#include <U8g2lib.h>

// ==========================================
// 音声波形プロッター設定
// ==========================================

// 1画面分のサンプル数
const int WAVE_POINTS = 256;

// 4サンプルごとに1点取得
// 44100 / 4 ≒ 11025 Hz
const int WAVE_DECIMATION = 4;

// 波形送信間隔（ms）
const unsigned long WAVE_INTERVAL = 60;

// PCM波形バッファ
int16_t waveBuffer[WAVE_POINTS];

int waveBufferIndex = 0;
bool waveBufferReady = false;

unsigned long lastWaveSend = 0;

// ==========================================
// 衝撃閾値（高感度設定）
// ==========================================
const int THRESHOLD_TOUCH = 80;   // 触れる
const int THRESHOLD_TRACE = 120;  // なぞる
const int THRESHOLD_STAB  = 150;  // 突き刺す
const int THRESHOLD_TEAR  = 200;  // 破る
const int THRESHOLD_CRUSH = 250;  // 壊す________________//////

// ==========================================
// 音響モジュール
// ==========================================
AudioSynthNoiseWhite    noise1;
AudioSynthWaveform      waveMain;
AudioSynthWaveform      waveSub;

AudioMixer4             oscMixer;
AudioMixer4             mainMixer;

AudioEffectEnvelope     envelope1;
AudioFilterLadder       ladder1;
AudioEffectBitcrusher   bitcrusher1;
AudioEffectDelay        delay1;

AudioMixer4             delayMixer;
AudioEffectFreeverb     freeverb1;

AudioOutputUSB          usb1;
AudioOutputI2S          i2sClock;

AudioAnalyzePeak        peak1;

// ★追加：実際の音声サンプルを取得
AudioRecordQueue        audioQueue;

// ==========================================
// Audio Routing
// ==========================================
AudioConnection patchCord1(
  waveMain, 0, oscMixer, 0
);

AudioConnection patchCord2(
  waveSub, 0, oscMixer, 1
);

AudioConnection patchCord3(
  oscMixer, 0, bitcrusher1, 0
);

AudioConnection patchCord4(
  bitcrusher1, 0, ladder1, 0
);

AudioConnection patchCord5(
  ladder1, 0, mainMixer, 0
);

AudioConnection patchCord6(
  noise1, 0, mainMixer, 1
);

AudioConnection patchCord7(
  mainMixer, envelope1
);

AudioConnection patchCord8(
  envelope1, 0, delayMixer, 0
);

AudioConnection patchCord9(
  delayMixer, 0, delay1, 0
);

AudioConnection patchCord10(
  delay1, 0, delayMixer, 1
);

AudioConnection patchCord11(
  delayMixer, freeverb1
);

// USB Audio出力
AudioConnection patchCord12(
  freeverb1, 0, usb1, 0
);

AudioConnection patchCord13(
  freeverb1, 0, usb1, 1
);

// 音量測定
AudioConnection patchCord14(
  freeverb1, 0, peak1, 0
);

// ★追加：音声波形の取得
AudioConnection patchCord15(
  freeverb1, 0, audioQueue, 0
);

// ★追加：Audio Libraryのクロック生成
AudioConnection patchCord16(
  freeverb1, 0, i2sClock, 0
);

AudioConnection patchCord17(
  freeverb1, 0, i2sClock, 1
);

// ==========================================
// OLED
// ==========================================
U8G2_SSD1309_128X64_NONAME0_F_4W_HW_SPI
  u8g2(U8G2_R0, 10, 9, 8);

// ==========================================
// ハードウェアピン
// ==========================================
const int MUX_SIG_PINS[6] = {
  A10, A11, A12, A13, A14, A15
};

const int MUX_S0 = 2;
const int MUX_S1 = 3;
const int MUX_S2 = 4;
const int MUX_S3 = 5;

const int POT_PINS[10] = {
  A0, A1, A2, A3, A4,
  A5, A6, A16, A8, A17
};

const int BTN_PINS[10] = {
  28, 29, 30, 31, 32,
  33, 34, 35, 36, 37
};

// ==========================================
// システム状態
// ==========================================
enum SystemMode {
  MODE_RESET,
  MODE_EDIT,
  MODE_LOCKED,
  MODE_MEMORY,
  MODE_SOMEONE
};

SystemMode currentMode = MODE_EDIT;

String displayMessage = "音作成モード";

unsigned long messageTimer = 0;
unsigned long lastActiveTime = 0;
unsigned long lastDisplayUpdate = 0;

bool isTouching = false;
bool prevBtnStates[10] = {false};

float params[10] = {
  0.5, 0.5, 0.5, 0.5, 0.5,
  0.5, 0.5, 0.5, 0.5, 0.5
};

// ★ツマミ変更検知・OLED表示用
float lastPotValues[10] = {
  0.5, 0.5, 0.5, 0.5, 0.5,
  0.5, 0.5, 0.5, 0.5, 0.5
};

int activePotIndex = -1;
// ノイズによる誤検知を防ぐため、変化検知のしきい値を少し高めに設定（5%以上の変化で反応）
const float POT_CHANGE_THRESHOLD = 0.05f;

const char* const POT_QUESTIONS[10] = {
  "丸い / 尖る",
  "ひとり / だれか",
  "隠れる / 目立つ",
  "浮く / 沈む",
  "すっきり/ぐちゃぐちゃ",
  "忘れる / 引きずる",
  "冷たい / あたたかい",
  "カツカツ / ゆとり",
  "漂う / 刻む",
  "馴染む / はみだす"
};

const char* const POT_NAMES[10] = {
  "A0",
  "A1",
  "A2",
  "A3",
  "A4",
  "A5",
  "A6",
  "A16(A7)",
  "A8",
  "A17(A9)"
};

// 「記憶モード」で保存し「誰かモード」で呼び出すパラメータ
float savedParams[10] = {
  0.75, 0.85, 0.70, 0.30, 0.65,
  0.80, 0.25, 0.20, 0.80, 0.60
};

// ==========================================
// ★記憶ファイル管理（重複ファイルの自動削除）
// ==========================================
const int MAX_MEMORY_FILES = 32;
const float PARAM_MATCH_EPSILON = 0.01f;

struct ParamFile {
  bool active;
  char filename[16];
  float values[10];
};

ParamFile memoryFiles[MAX_MEMORY_FILES];
int memoryFileCount = 0;
int nextFileId = 1;
int currentSomeoneIndex = 0;
bool sdAvailable = false;

int fakeTriggerForce = 0;
float currentPeak = 0.0;

// ==========================================
// ピエゾセンサー
// ==========================================
const int NUM_PIEZOS = 48;
const int BUFFER_SIZE = 16;

int buffers[NUM_PIEZOS][BUFFER_SIZE];
int bufferIndex = 0;

int maxDelta = 0;

// ==========================================
// 浮動小数点 map
// ==========================================
float mapFloat(
  float x,
  float inMin,
  float inMax,
  float outMin,
  float outMax
) {
  return (x - inMin) *
         (outMax - outMin) /
         (inMax - inMin) + outMin;
}

// ==========================================
// ★2つのファイルの全10パラメータが同一か判定
// ==========================================
bool isSameParameterSet(
  const float a[10],
  const float b[10]
) {
  for (int p = 0; p < 10; p++) {
    if (fabs(a[p] - b[p]) > PARAM_MATCH_EPSILON) {
      return false;
    }
  }
  return true;
}

// ==========================================
// ★全て同じパラメータを持つ重複ファイルを自動削除
// ==========================================
int removeDuplicateFiles() {

  int removedCount = 0;

  for (int i = 0; i < memoryFileCount; i++) {

    for (int j = i + 1; j < memoryFileCount; ) {

      if (
        isSameParameterSet(
          memoryFiles[i].values,
          memoryFiles[j].values
        )
      ) {

        // SDカード上の重複ファイルも削除
        if (
          sdAvailable &&
          SD.exists(memoryFiles[j].filename)
        ) {
          SD.remove(memoryFiles[j].filename);
        }

        // リストを詰めて重複分を削除（1つだけ残す）
        for (int k = j; k < memoryFileCount - 1; k++) {
          memoryFiles[k] = memoryFiles[k + 1];
        }

        memoryFileCount--;
        removedCount++;

      } else {
        j++;
      }
    }
  }

  if (
    currentSomeoneIndex >= memoryFileCount &&
    memoryFileCount > 0
  ) {
    currentSomeoneIndex = 0;
  }

  return removedCount;
}

// ==========================================
// ★記憶モード：新規ファイル保存＋重複の自動削除
// ==========================================
void saveMemoryFile() {

  // 上限に達している場合は最も古いファイルを削除して空ける
  if (memoryFileCount >= MAX_MEMORY_FILES) {

    if (
      sdAvailable &&
      SD.exists(memoryFiles[0].filename)
    ) {
      SD.remove(memoryFiles[0].filename);
    }

    for (int k = 0; k < memoryFileCount - 1; k++) {
      memoryFiles[k] = memoryFiles[k + 1];
    }

    memoryFileCount = MAX_MEMORY_FILES - 1;
  }

  ParamFile &newFile = memoryFiles[memoryFileCount];
  newFile.active = true;
  snprintf(
    newFile.filename,
    sizeof(newFile.filename),
    "MEM_%03d.TXT",
    nextFileId++
  );

  for (int p = 0; p < 10; p++) {
    newFile.values[p] = params[p];
    savedParams[p] = params[p];
  }

  if (sdAvailable) {
    File f = SD.open(newFile.filename, FILE_WRITE);
    if (f) {
      for (int p = 0; p < 10; p++) {
        f.println(newFile.values[p], 4);
      }
      f.close();
    }
  }

  memoryFileCount++;

  // 全て同じパラメータを持つファイルが2つ以上ある場合、自動的に削除
  removeDuplicateFiles();
}

// ==========================================
// ★起動時の記憶ファイル初期化・重複クリーンアップ
// ==========================================
void initializeMemoryFiles() {

#ifdef BUILTIN_SDCARD
  sdAvailable = SD.begin(BUILTIN_SDCARD);
#else
  sdAvailable = false;
#endif

  if (sdAvailable) {
    File root = SD.open("/");
    if (root) {
      while (true) {
        File entry = root.openNextFile();
        if (!entry) break;

        if (!entry.isDirectory()) {
          const char *name = entry.name();
          if (
            strncmp(name, "MEM_", 4) == 0 &&
            memoryFileCount < MAX_MEMORY_FILES
          ) {
            ParamFile &pf = memoryFiles[memoryFileCount];
            pf.active = true;
            strncpy(
              pf.filename,
              name,
              sizeof(pf.filename) - 1
            );
            pf.filename[sizeof(pf.filename) - 1] = '\0';

            for (int p = 0; p < 10; p++) {
              if (entry.available()) {
                pf.values[p] = entry.parseFloat();
              } else {
                pf.values[p] = 0.5f;
              }
            }
            memoryFileCount++;
            nextFileId++;
          }
        }
        entry.close();
      }
      root.close();
    }
  }

  // 起動時にも同一パラメータの重複ファイルが2つ以上あれば自動削除
  removeDuplicateFiles();

  // ファイルが1つもない場合は初期の「誰か」パラメータを1件登録
  if (memoryFileCount == 0) {
    memoryFileCount = 1;
    memoryFiles[0].active = true;
    snprintf(
      memoryFiles[0].filename,
      sizeof(memoryFiles[0].filename),
      "MEM_000.TXT"
    );
    for (int p = 0; p < 10; p++) {
      memoryFiles[0].values[p] = savedParams[p];
    }
  }
}

// ==========================================
// MUXチャンネル選択
// ==========================================
void selectMuxChannel(int channel) {

  digitalWrite(MUX_S0, bitRead(channel, 0));
  digitalWrite(MUX_S1, bitRead(channel, 1));
  digitalWrite(MUX_S2, bitRead(channel, 2));
  digitalWrite(MUX_S3, bitRead(channel, 3));

  delayMicroseconds(10);
}

// ==========================================
// ピエゾ初期化
// ==========================================
void initializePiezoBuffers() {

  for (int ch = 0; ch < 8; ch++) {

    selectMuxChannel(ch);

    for (int m = 0; m < 6; m++) {

      int index = m * 8 + ch;
      int value = analogRead(MUX_SIG_PINS[m]);

      for (int i = 0; i < BUFFER_SIZE; i++) {
        buffers[index][i] = value;
      }
    }
  }
}

// ==========================================
// Setup
// ==========================================
void setup() {

  Serial.begin(115200);

  analogReadResolution(10);

  // 音声用メモリ
  AudioMemory(100);

  // ★音声キャプチャ開始
  audioQueue.begin();

  // OLED
  u8g2.begin();
  u8g2.enableUTF8Print();

  // MUX
  for (int i = 0; i < 6; i++) {
    pinMode(MUX_SIG_PINS[i], INPUT);
  }

  pinMode(MUX_S0, OUTPUT);
  pinMode(MUX_S1, OUTPUT);
  pinMode(MUX_S2, OUTPUT);
  pinMode(MUX_S3, OUTPUT);

  // ボタン
  for (int i = 0; i < 10; i++) {
    pinMode(BTN_PINS[i], INPUT_PULLUP);
  }

  // 起動時のツマミ初期位置を記録
  for (int i = 0; i < 10; i++) {
    float initVal = analogRead(POT_PINS[i]) / 1023.0f;
    params[i] = initVal;
    lastPotValues[i] = initVal;
  }

  initializePiezoBuffers();
  initializeMemoryFiles();

  // ========================================
  // 音響初期化
  // ========================================

  waveMain.begin(WAVEFORM_SAWTOOTH);
  waveSub.begin(WAVEFORM_SAWTOOTH);

  noise1.amplitude(1.0);

  oscMixer.gain(0, 1.0);
  oscMixer.gain(1, 0.5);

  mainMixer.gain(0, 1.0);
  mainMixer.gain(1, 0.0);

  delayMixer.gain(0, 1.0);
  delayMixer.gain(1, 0.0);

  ladder1.frequency(2500);
  ladder1.resonance(0.5);

  bitcrusher1.bits(16);
  bitcrusher1.sampleRate(44100);

  // ディレイ時間
  delay1.delay(0, 240);

  freeverb1.roomsize(0.6);
  freeverb1.damping(0.2);

  // エンベロープ
  envelope1.attack(20);
  envelope1.decay(100);
  envelope1.sustain(0.8);
  envelope1.release(400);

  lastWaveSend = millis();
}

// ==========================================
// ボタン入力
// ==========================================
void updateButtons(unsigned long currentTime) {

  fakeTriggerForce = 0;

  for (int i = 0; i < 10; i++) {

    bool isPressed =
      (digitalRead(BTN_PINS[i]) == LOW);

    if (isPressed && !prevBtnStates[i]) {

      activePotIndex = -1;

      switch (i) {

        case 0:
          currentMode = MODE_RESET;
          for (int p = 0; p < 10; p++) {
            params[p] = 0.5f;
          }
          displayMessage = "[ 私 ]";
          messageTimer = currentTime;
          break;

        case 1:
          currentMode = MODE_EDIT;
          for (int p = 0; p < 10; p++) {
            lastPotValues[p] = params[p];
          }
          displayMessage = "音作成モード";
          messageTimer = currentTime;
          break;

        case 2:
          fakeTriggerForce = THRESHOLD_TOUCH;
          displayMessage = "触れる";
          messageTimer = currentTime;
          break;

        case 3:
          fakeTriggerForce = THRESHOLD_TRACE;
          displayMessage = "なぞる";
          messageTimer = currentTime;
          break;

        case 4:
          fakeTriggerForce = THRESHOLD_STAB;
          displayMessage = "突き刺す";
          messageTimer = currentTime;
          break;

        case 5:
          fakeTriggerForce = THRESHOLD_TEAR;
          displayMessage = "破る";
          messageTimer = currentTime;
          break;

        case 6:
          fakeTriggerForce = THRESHOLD_CRUSH;
          displayMessage = "壊す";
          messageTimer = currentTime;
          break;

        case 7:
          currentMode = MODE_LOCKED;
          displayMessage = "固定中";
          messageTimer = currentTime;
          break;

        case 8:
          currentMode = MODE_MEMORY;
          saveMemoryFile();
          displayMessage = "記憶モード";
          messageTimer = currentTime;
          break;

        case 9:
          currentMode = MODE_SOMEONE;
          removeDuplicateFiles();
          if (memoryFileCount > 0) {
            currentSomeoneIndex =
              (currentSomeoneIndex + 1) % memoryFileCount;
            for (int p = 0; p < 10; p++) {
              params[p] =
                memoryFiles[currentSomeoneIndex].values[p];
              savedParams[p] = params[p];
            }
          }
          displayMessage = "[ 誰か ]";
          messageTimer = currentTime;
          break;
      }
    }

    prevBtnStates[i] = isPressed;
  }
}

// ==========================================
// ツマミ入力（感度を改善しノイズによる誤表示を防止）
// ==========================================
void updatePots() {

  if (currentMode != MODE_EDIT) {
    return;
  }

  unsigned long now = millis();

  for (int i = 0; i < 10; i++) {

    float raw = analogRead(POT_PINS[i]) / 1023.0f;
    params[i] = params[i] * 0.8f + raw * 0.2f;

    // 設定値（5%以上）を変更したときだけOLEDの表示を切り替える
    if (fabs(params[i] - lastPotValues[i]) >= POT_CHANGE_THRESHOLD) {
      lastPotValues[i] = params[i];
      activePotIndex = i;
      displayMessage = POT_QUESTIONS[i];
      messageTimer = now;
    }
  }
}

// ==========================================
// 音響パラメータ
// ==========================================
void updateAudioParameters(unsigned long currentTime) {

  if (params[0] < 0.2f && params[8] < 0.4f) {
    waveMain.begin(WAVEFORM_SINE);
  } else if (params[0] > 0.65f || params[8] > 0.7f) {
    waveMain.begin(WAVEFORM_SAWTOOTH);
  } else {
    waveMain.begin(WAVEFORM_TRIANGLE);
  }

  int bits = (int)mapFloat(
    params[0], 0.0f, 1.0f, 16.0f, 3.0f
  );
  float crushRate = mapFloat(
    params[0], 0.0f, 1.0f, 44100.0f, 2400.0f
  );

  bitcrusher1.bits(bits);
  bitcrusher1.sampleRate(crushRate);

  freeverb1.roomsize(mapFloat(params[1], 0.0f, 1.0f, 0.0f, 0.98f));
  freeverb1.damping(mapFloat(params[1], 0.0f, 1.0f, 0.85f, 0.05f));

  oscMixer.gain(1, mapFloat(params[3], 0.0f, 1.0f, 0.05f, 1.0f));

  float chaos1 = sin(currentTime / 170.0f);
  float chaos2 = sin(currentTime / 43.0f);
  float chaos3 = cos(currentTime / 19.0f);
  float wobble =
    (chaos1 * 0.5f + chaos2 * 0.3f + chaos3 * 0.2f) *
    (params[4] * params[4] * 140.0f);

  delayMixer.gain(1, mapFloat(params[5], 0.0f, 1.0f, 0.0f, 0.93f));

  envelope1.attack(
    mapFloat(params[7], 0.0f, 1.0f, 2.0f, 900.0f)
  );
  envelope1.decay(
    mapFloat(params[7], 0.0f, 1.0f, 30.0f, 400.0f)
  );
  envelope1.sustain(
    mapFloat(params[7], 0.0f, 1.0f, 0.4f, 0.9f)
  );
  envelope1.release(
    mapFloat(params[7], 0.0f, 1.0f, 90.0f, 1200.0f)
  );

  float vitality = mapFloat(
    params[8], 0.0f, 1.0f, 0.15f, 0.90f
  );

  if (params[8] > 0.6f) {
    waveSub.begin(WAVEFORM_SQUARE);
  } else {
    waveSub.begin(WAVEFORM_SAWTOOTH);
  }

  float gravityPitch = mapFloat(
    params[3], 0.0f, 1.0f, 440.0f, 165.0f
  );

  float basePitch = gravityPitch + wobble;
  if (basePitch < 40.0f) {
    basePitch = 40.0f;
  }

  float cleanHarmony = (params[3] < 0.5f) ? 1.5f : 0.5f;
  float outcastDetune = params[9] * 0.21f;

  waveMain.frequency(basePitch);

  waveSub.frequency(
    basePitch * (cleanHarmony + outcastDetune)
  );

  waveMain.amplitude(vitality);
  waveSub.amplitude(vitality);
}

// ==========================================
// ピエゾスキャン
// ==========================================
void scanPiezos() {

  maxDelta = 0;

  for (int ch = 0; ch < 8; ch++) {

    selectMuxChannel(ch);

    for (int m = 0; m < 6; m++) {

      int piezoIndex = m * 8 + ch;

      int rawValue =
        analogRead(MUX_SIG_PINS[m]);

      buffers[piezoIndex][bufferIndex] =
        rawValue;

      int minVal = 1023;
      int maxVal = 0;

      for (int i = 0; i < BUFFER_SIZE; i++) {

        int value = buffers[piezoIndex][i];

        if (value > maxVal) {
          maxVal = value;
        }

        if (value < minVal) {
          minVal = value;
        }
      }

      int delta = maxVal - minVal;

      if (delta > maxDelta) {
        maxDelta = delta;
      }
    }
  }

  bufferIndex =
    (bufferIndex + 1) % BUFFER_SIZE;
}

// ==========================================
// 衝撃による音響処理
// ==========================================
void processImpact(
  int finalImpact,
  unsigned long currentTime
) {

  float baseResonance = mapFloat(
    params[2], 0.0f, 1.0f, 0.05f, 0.92f
  );

  float prideCutoff = mapFloat(
    params[2], 0.0f, 1.0f, 220.0f, 6500.0f
  );

  float warmthFactor = mapFloat(
    params[6], 0.0f, 1.0f, 1.8f, 0.55f
  );
  float baseCutoff = prideCutoff * warmthFactor;

  float warmthResMod = mapFloat(
    params[6], 0.0f, 1.0f, 0.1f, -0.15f
  );
  baseResonance = constrain(
    baseResonance + warmthResMod, 0.05f, 0.94f
  );

  if (finalImpact >= THRESHOLD_TOUCH) {

    float impactNoise = 0.0;
    float impactCutoffAdd = 0.0;
    float impactResAdd = 0.0;

    float sharpness =
      mapFloat(params[0], 0.0f, 1.0f, 0.3f, 1.5f) *
      mapFloat(params[6], 0.0f, 1.0f, 1.3f, 0.4f);

    if (finalImpact >= THRESHOLD_CRUSH) {

      impactNoise = constrain(0.45f * sharpness, 0.0f, 1.0f);
      impactCutoffAdd = 9000;
      impactResAdd = 0.45;

    } else if (finalImpact >= THRESHOLD_TEAR) {

      impactNoise = constrain(0.30f * sharpness, 0.0f, 1.0f);
      impactCutoffAdd = 7000;
      impactResAdd = 0.35;

    } else if (finalImpact >= THRESHOLD_STAB) {

      impactNoise = constrain(0.15f * sharpness, 0.0f, 1.0f);
      impactCutoffAdd = 5000;
      impactResAdd = 0.25;

    } else if (finalImpact >= THRESHOLD_TRACE) {

      impactNoise = constrain(0.05f * sharpness, 0.0f, 1.0f);
      impactCutoffAdd = 3000;
      impactResAdd = 0.15;

    } else {

      impactNoise = 0.0;
      impactCutoffAdd = 1200;
      impactResAdd = 0.05;
    }

    mainMixer.gain(1, impactNoise);

    float claimScale = mapFloat(
      params[2], 0.0f, 1.0f, 0.25f, 1.2f
    );
    float targetCutoff =
      baseCutoff + (impactCutoffAdd * claimScale);

    if (targetCutoff > 14000.0f) {
      targetCutoff = 14000.0f;
    }

    ladder1.frequency(targetCutoff);

    float resonance =
      baseResonance + (impactResAdd * claimScale);

    if (resonance > 0.95f) {
      resonance = 0.95f;
    }

    ladder1.resonance(resonance);

    if (!isTouching) {

      envelope1.noteOn();
      isTouching = true;
    }

    lastActiveTime = currentTime;

  } else {

    mainMixer.gain(1, 0.0);

    ladder1.frequency(baseCutoff);
    ladder1.resonance(baseResonance);

    if (
      isTouching &&
      currentTime - lastActiveTime > 300
    ) {

      envelope1.noteOff();
      isTouching = false;
    }
  }
}

// ==========================================
// ★生成音のPCM波形をキャプチャ
// ==========================================
void captureAudioWaveform() {

  while (audioQueue.available() > 0) {

    int16_t *samples =
      audioQueue.readBuffer();

    if (!waveBufferReady) {

      for (
        int i = 0;
        i < AUDIO_BLOCK_SAMPLES;
        i += WAVE_DECIMATION
      ) {

        waveBuffer[waveBufferIndex] =
          samples[i];

        waveBufferIndex++;

        if (waveBufferIndex >= WAVE_POINTS) {

          waveBufferReady = true;
          break;
        }
      }
    }

    audioQueue.freeBuffer();
  }
}

// ==========================================
// ★波形をシリアルプロッターへ送信
// ==========================================
void sendAudioWaveform(unsigned long currentTime) {

  if (!waveBufferReady) {
    return;
  }

  if (
    currentTime - lastWaveSend <
    WAVE_INTERVAL
  ) {
    return;
  }

  lastWaveSend = currentTime;

  if (Serial) {

    for (int i = 0; i < WAVE_POINTS; i++) {

      Serial.println(waveBuffer[i]);
    }
  }

  waveBufferIndex = 0;
  waveBufferReady = false;
}

// ==========================================
// OLED
// ==========================================
void updateDisplay(unsigned long currentTime) {

  if (
    currentTime - lastDisplayUpdate < 50
  ) {
    return;
  }

  lastDisplayUpdate = currentTime;

  if (currentTime - messageTimer > 2000) {

    activePotIndex = -1;

    switch (currentMode) {

      case MODE_EDIT:
        displayMessage = "音作成モード";
        break;

      case MODE_LOCKED:
        displayMessage = "固定中";
        break;

      case MODE_RESET:
        displayMessage = "[ 私 ]";
        break;

      case MODE_MEMORY:
        displayMessage = "記憶モード";
        break;

      case MODE_SOMEONE:
        displayMessage = "[ だれか ]";
        break;
    }
  }

  u8g2.clearBuffer();

  u8g2.setFont(
    u8g2_font_b12_t_japanese2
  );

  int mainWidth =
    u8g2.getUTF8Width(
      displayMessage.c_str()
    );

  u8g2.setCursor(
    (128 - mainWidth) / 2,
    20
  );

  u8g2.print(displayMessage);

  u8g2.setFont(u8g2_font_6x10_tf);

  // 音量
  u8g2.setCursor(0, 48);
  u8g2.print("VOL");

  int barLength =
    (int)(currentPeak * 35.0f);

  if (barLength > 35) {
    barLength = 35;
  }

  if (barLength < 0) {
    barLength = 0;
  }

  u8g2.drawFrame(22, 39, 37, 10);
  u8g2.drawBox(23, 40, barLength, 8);

  // 衝撃値
  u8g2.setCursor(65, 48);
  u8g2.print("S:");
  u8g2.print(maxDelta);

  // ツマミ名と現在値(%)を下段に表示
  if (activePotIndex >= 0 && activePotIndex < 10) {
    int pct = (int)(params[activePotIndex] * 100.0f + 0.5f);
    u8g2.setCursor(0, 62);
    u8g2.print("KNOB ");
    u8g2.print(POT_NAMES[activePotIndex]);
    u8g2.print(" : ");
    u8g2.print(pct);
    u8g2.print("%");
  }

  u8g2.sendBuffer();
}

// ==========================================
// Main Loop
// ==========================================
void loop() {

  unsigned long currentTime = millis();

  // 1. ボタン
  updateButtons(currentTime);

  // 2. ツマミ
  updatePots();

  // 3. 音響パラメータ
  updateAudioParameters(currentTime);

  // 4. ピエゾ
  scanPiezos();

  // 5. 衝撃判定
  int finalImpact =
    max(maxDelta, fakeTriggerForce);

  // 6. 音響生成
  processImpact(finalImpact, currentTime);

  // 7. 実際の音量を取得
  if (peak1.available()) {
    currentPeak = peak1.read();
  }

  // 8. ★音声サンプル取得
  captureAudioWaveform();

  // 9. ★音声波形をプロット
  sendAudioWaveform(currentTime);

  // 10. OLED表示
  updateDisplay(currentTime);
}
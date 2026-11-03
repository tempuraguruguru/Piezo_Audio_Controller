
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
const int THRESHOLD_TOUCH = 50;   // 触れる
const int THRESHOLD_TRACE = 100;  // なぞる
const int THRESHOLD_STAB  = 200;  // 突き刺す
const int THRESHOLD_TEAR  = 350;  // 破る
const int THRESHOLD_CRUSH = 500;  // 壊す________________//////

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
  MODE_SOMEONE
};

SystemMode currentMode = MODE_EDIT;

String displayMessage = "設定モード";

unsigned long messageTimer = 0;
unsigned long lastActiveTime = 0;
unsigned long lastDisplayUpdate = 0;

bool isTouching = false;
bool prevBtnStates[10] = {false};

float params[10] = {
  0.5, 0.5, 0.5, 0.5, 0.5,
  0.5, 0.5, 0.5, 0.5, 0.5
};

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

  initializePiezoBuffers();

  // ========================================
  // 音響初期化
  // ========================================

  waveMain.begin(WAVEFORM_SAWTOOTH);
  waveSub.begin(WAVEFORM_SQUARE);

  noise1.amplitude(1.0);

  oscMixer.gain(0, 1.0);
  oscMixer.gain(1, 0.0);

  mainMixer.gain(0, 1.0);
  mainMixer.gain(1, 0.0);

  delayMixer.gain(0, 1.0);
  delayMixer.gain(1, 0.0);

  ladder1.frequency(1000);
  ladder1.resonance(0.5);

  bitcrusher1.bits(16);
  bitcrusher1.sampleRate(44100);

  // ディレイ時間
  delay1.delay(0, 180);

  freeverb1.roomsize(0.6);

  // エンベロープ
  envelope1.attack(20);
  envelope1.decay(50);
  envelope1.sustain(0.8);
  envelope1.release(300);

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

      switch (i) {

        case 0:
          currentMode = MODE_RESET;
          displayMessage = "[ 私 ]";
          messageTimer = currentTime;
          break;

        case 1:
          currentMode = MODE_EDIT;
          displayMessage = "設定モード";
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
      }
    }

    prevBtnStates[i] = isPressed;
  }
}

// ==========================================
// ツマミ入力
// ==========================================
void updatePots() {

  if (currentMode != MODE_EDIT) {
    return;
  }

  for (int i = 0; i < 10; i++) {

    // ツマミ接続時は次の行を有効化
    // params[i] = analogRead(POT_PINS[i]) / 1023.0f;

    params[i] = 0.5f;
  }
}

// ==========================================
// 音響パラメータ
// ==========================================
void updateAudioParameters(unsigned long currentTime) {

  int bits = (int)mapFloat(
    params[0], 0, 1, 16, 3
  );

  bitcrusher1.bits(bits);

  freeverb1.roomsize(params[1]);

  oscMixer.gain(1, params[3]);

  delayMixer.gain(1, params[5] * 0.8f);

  envelope1.attack(
    mapFloat(params[7], 0, 1, 5, 1000)
  );

  float vitality = mapFloat(
    params[8], 0, 1, 0.2f, 0.8f
  );

  float detune = mapFloat(
    params[9], 0, 1, 1.0f, 1.05f
  );

  float wobble =
    sin(currentTime / 200.0f) *
    (params[4] * 50.0f);

  float basePitch = 110.0f + wobble;

  waveMain.frequency(basePitch);

  waveSub.frequency(
    basePitch * 1.5f * detune
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
    params[2], 0, 1, 0.1f, 0.9f
  );

  float baseCutoff = mapFloat(
    params[6], 0, 1, 300, 2000
  );

  if (finalImpact >= THRESHOLD_TOUCH) {

    float impactNoise = 0.0;
    float impactCutoffAdd = 0.0;
    float impactResAdd = 0.0;

    if (finalImpact >= THRESHOLD_CRUSH) {

      impactNoise = 1.0;
      impactCutoffAdd = 10000;
      impactResAdd = 0.8;

    } else if (finalImpact >= THRESHOLD_TEAR) {

      impactNoise = 0.6;
      impactCutoffAdd = 8000;
      impactResAdd = 0.5;

    } else if (finalImpact >= THRESHOLD_STAB) {

      impactNoise = 0.2;
      impactCutoffAdd = 6000;
      impactResAdd = 0.3;

    } else if (finalImpact >= THRESHOLD_TRACE) {

      impactNoise = 0.05;
      impactCutoffAdd = 3000;
      impactResAdd = 0.1;

    } else {

      impactNoise = 0.0;
      impactCutoffAdd = 1000;
      impactResAdd = 0.0;
    }

    mainMixer.gain(1, impactNoise);

    ladder1.frequency(
      baseCutoff + impactCutoffAdd
    );

    float resonance =
      baseResonance + impactResAdd;

    if (resonance > 1.0f) {
      resonance = 1.0f;
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

  // キューに届いた128サンプル単位の
  // 音声データを取り出す
  while (audioQueue.available() > 0) {

    int16_t *samples =
      audioQueue.readBuffer();

    if (!waveBufferReady) {

      for (
        int i = 0;
        i < AUDIO_BLOCK_SAMPLES;
        i += WAVE_DECIMATION
      ) {

        // 実際の16bit PCM値
        waveBuffer[waveBufferIndex] =
          samples[i];

        waveBufferIndex++;

        if (waveBufferIndex >= WAVE_POINTS) {

          waveBufferReady = true;
          break;
        }
      }
    }

    // AudioMemoryへ返却
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

    // 256個のPCM値を連続出力
    // 1行 = 波形の1サンプル
    for (int i = 0; i < WAVE_POINTS; i++) {

      Serial.println(waveBuffer[i]);
    }
  }

  // 次の波形取得へ
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

    switch (currentMode) {

      case MODE_EDIT:
        displayMessage = "設定モード";
        break;

      case MODE_LOCKED:
        displayMessage = "固定中";
        break;

      case MODE_RESET:
        displayMessage = "[ 私 ]";
        break;

      case MODE_SOMEONE:
        displayMessage = "[ 誰か ]";
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

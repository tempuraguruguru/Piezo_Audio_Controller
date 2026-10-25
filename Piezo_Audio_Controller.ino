#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <SerialFlash.h>
#include <U8g2lib.h> 

// --- Audio System ---
AudioSynthNoiseWhite     noise1;
AudioSynthWaveform       wave1; // Root
AudioSynthWaveform       wave2; // 3rd
AudioSynthWaveform       wave3; // 5th
AudioSynthWaveform       wave4; // Octave
AudioSynthWaveform       wave5; // Sub

AudioMixer4              mixer1; 
AudioMixer4              mixer2; 
AudioEffectEnvelope      envelope1;
AudioFilterLadder        ladder1;
AudioEffectBitcrusher    bitcrusher1;
AudioEffectFreeverb      freeverb1;
AudioOutputUSB           usb1;

AudioConnection          patchCord1(wave1, 0, mixer1, 0);
AudioConnection          patchCord2(wave2, 0, mixer1, 1);
AudioConnection          patchCord3(wave3, 0, mixer1, 2);
AudioConnection          patchCord4(wave4, 0, mixer1, 3);
AudioConnection          patchCord5(mixer1, 0, mixer2, 0);
AudioConnection          patchCord6(wave5, 0, mixer2, 1);
AudioConnection          patchCord7(noise1, 0, mixer2, 2);
AudioConnection          patchCord8(mixer2, envelope1);
AudioConnection          patchCord9(envelope1, 0, ladder1, 0);
AudioConnection          patchCord10(ladder1, bitcrusher1);
AudioConnection          patchCord11(bitcrusher1, freeverb1);
AudioConnection          patchCord12(freeverb1, 0, usb1, 0);
AudioConnection          patchCord13(freeverb1, 0, usb1, 1);

// --- OLED Display ---
U8G2_SSD1309_128X64_NONAME0_F_4W_HW_SPI u8g2(U8G2_R0, 10, 9, 8);

// --- Pins: 10 Knobs & 10 Switches ---
const int MUX_SIG = A10; 
const int MUX_S0 = 2; const int MUX_S1 = 3; 
const int MUX_S2 = 4; const int MUX_S3 = 5;

const int POT_PINS[10] = {A0, A1, A2, A3, A4, A5, A6, A7, A8, A9};
const int BTN_PINS[10] = {28, 29, 30, 31, 32, 33, 34, 35, 36, 37};

int potValues[10] = {0};
bool btnStates[10] = {false};
bool prevBtnStates[10] = {false};
bool activeToggles[10] = {false}; 

float oscRatios[5] = {1.0, 1.25, 1.5, 2.0, 0.5}; // Root, Major3rd, Perfect5th, OctaveUp, Sub
AudioSynthWaveform* waves[5] = {&wave1, &wave2, &wave3, &wave4, &wave5};

// 2文字の抽象ラベル (OLED表示用)
const char* labels[10] = {"AG", "MD", "EG", "TM", "AU", "FL", "LF", "FE", "WV", "PR"};

// --- Sensor Variables ---
const int NUM_CHANNELS = 6; 
const int BUFFER_SIZE = 16; 
int buffers[NUM_CHANNELS][BUFFER_SIZE];
int bufferIndex = 0;

unsigned long lastStabTime = 0;
bool isTouching = false;
unsigned long lastActiveTime = 0;
unsigned long lastDisplayUpdate = 0;

void setup() {
  AudioMemory(30); 
  u8g2.begin();

  pinMode(MUX_SIG, INPUT);
  pinMode(MUX_S0, OUTPUT); pinMode(MUX_S1, OUTPUT);
  pinMode(MUX_S2, OUTPUT); pinMode(MUX_S3, OUTPUT);

  for (int i = 0; i < 10; i++) {
    pinMode(BTN_PINS[i], INPUT_PULLUP); 
    if(i < 5) {
      waves[i]->begin(WAVEFORM_SAWTOOTH); 
      waves[i]->amplitude(0.0);
    }
  }

  noise1.amplitude(1.0); 
  for(int i=0; i<4; i++) mixer1.gain(i, 1.0);
  mixer2.gain(0, 1.0); mixer2.gain(1, 1.0); mixer2.gain(2, 0.0); 

  envelope1.sustain(0.8);
  bitcrusher1.sampleRate(44100);
  freeverb1.damping(0.2);
}

void selectMuxChannel(int channel) {
  digitalWrite(MUX_S0, bitRead(channel, 0));
  digitalWrite(MUX_S1, bitRead(channel, 1));
  digitalWrite(MUX_S2, bitRead(channel, 2));
  digitalWrite(MUX_S3, bitRead(channel, 3));
  delayMicroseconds(5); 
}

void loop() {
  unsigned long currentTime = millis();
  int activeOscCount = 0; 

  // ==========================================
  // 1. 鑑賞者の状態（10ピン）読み取り
  // ==========================================
  for(int i = 0; i < 10; i++) {
    potValues[i] = analogRead(POT_PINS[i]);
    bool isPressed = (digitalRead(BTN_PINS[i]) == LOW);
    
    if(isPressed && !prevBtnStates[i]) {
      activeToggles[i] = !activeToggles[i]; // トグル動作
    }
    prevBtnStates[i] = isPressed;
    if(i < 5 && activeToggles[i]) activeOscCount++;
  }

  // --- 10の抽象パラメータを音響へマッピング ---
  float pAge   = map(potValues[0], 0, 1023, 40, 1000);   // AG: Base Pitch
  float pMind  = map(potValues[1], 0, 1023, 100, 10000); // MD: Filter Cutoff
  float pEgo   = potValues[2] / 1023.0;                  // EG: Resonance
  float pTime  = map(potValues[3], 0, 1023, 50, 8000);   // TM: Release Time
  float pAura  = potValues[4] / 1023.0;                  // AU: Reverb Size
  int   pFlaw  = map(potValues[5], 0, 1023, 16, 2);      // FL: Bitcrush (16 to 2 bits)
  float pLife  = potValues[6] / 1023.0;                  // LF: Noise Level
  float pFeel  = map(potValues[7], 0, 1023, 10, 3000);   // FE: Attack Time
  float pWavy  = potValues[8] / 1023.0;                  // WV: Pitch Wobble Magnitude
  float pPres  = potValues[9] / 1023.0;                  // PR: Master Volume

  // 音色への適用
  envelope1.attack(pFeel);
  envelope1.release(pTime);
  freeverb1.roomsize(pAura);
  bitcrusher1.bits(activeToggles[6] ? 3 : pFlaw); // Btn6: Glitch Mode

  // ピッチの揺らぎ（迷い）の計算
  float wobbleOffset = (sin(currentTime / 500.0) * (pWavy * 100.0));
  float finalPitch = pAge + wobbleOffset;

  for(int i = 0; i < 5; i++) {
    waves[i]->frequency(finalPitch * oscRatios[i]);
    waves[i]->amplitude(activeToggles[i] ? (0.15 * pPres) : 0.0); 
  }

  // ==========================================
  // 2. センサー読み取り（外界からの接触）
  // ==========================================
  int maxDelta = 0; 
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    selectMuxChannel(ch); 
    int rawValue = analogRead(MUX_SIG);
    buffers[ch][bufferIndex] = rawValue;
    
    int minVal = 1023; int maxVal = 0;
    for (int i = 0; i < BUFFER_SIZE; i++) {
      if (buffers[ch][i] > maxVal) maxVal = buffers[ch][i];
      if (buffers[ch][i] < minVal) minVal = buffers[ch][i];
    }
    int delta = maxVal - minVal;
    if (delta > maxDelta) maxDelta = delta; 
  }
  bufferIndex = (bufferIndex + 1) % BUFFER_SIZE;

  // ==========================================
  // 3. 状態遷移 (自己と外界の反応)
  // ==========================================
  bool isDrone = activeToggles[5]; // Btn5: 永遠の持続モード

  if (maxDelta > 150) { 
    ladder1.frequency(12000); // 強い刺激で思考が限界まで開く
    ladder1.resonance(pEgo);
    mixer2.gain(2, pLife * pPres); 
    if(activeOscCount == 0) envelope1.noteOn(); 
    lastStabTime = currentTime;
    lastActiveTime = currentTime;
    isTouching = true;
  }
  else if (maxDelta > 30) {
    if(activeOscCount == 0 && !isTouching) { envelope1.noteOn(); isTouching = true; } 
    lastActiveTime = currentTime;
    
    float dynamicCutoff = pMind + map(maxDelta, 30, 100, 0, 3000);
    if (dynamicCutoff > 10000) dynamicCutoff = 10000;
    ladder1.frequency(dynamicCutoff);
    ladder1.resonance(pEgo);
    mixer2.gain(2, (pLife * 0.5) * pPres); 
  }
  else {
    if (activeOscCount > 0 || isDrone) {
      if (!isTouching) { envelope1.noteOn(); isTouching = true; }
      ladder1.frequency(pMind); 
      ladder1.resonance(pEgo);
      mixer2.gain(2, (pLife * 0.1) * pPres); 
      lastActiveTime = currentTime;
    } else {
      if (maxDelta > 15) { 
        // ノイズフロア無視
      } else if (isTouching && (currentTime - lastActiveTime > 200)) {
        envelope1.noteOff(); 
        isTouching = false;
      }
    }
  }

  // ==========================================
  // 4. OLEDグラフィック描画 (10本のパラメーター柱)
  // ==========================================
  if (currentTime - lastDisplayUpdate > 60) {
    lastDisplayUpdate = currentTime;
    
    u8g2.clearBuffer(); 
    u8g2.setFont(u8g2_font_5x7_tr); 

    // 画面を10列(12ピクセル刻み)に分割して全情報を描画
    for(int i = 0; i < 10; i++) {
      int x = 4 + (i * 12); 

      // 上段: ボタンの状態 (四角形のON/OFF)
      if(activeToggles[i]) {
        u8g2.drawBox(x, 0, 10, 8); 
      } else {
        u8g2.drawFrame(x, 0, 10, 8); 
      }

      // 中段: 概念ラベル (AG, MD, EG...)
      u8g2.setCursor(x, 18);
      u8g2.print(labels[i]);

      // 下段: ツマミの状態 (縦のプログレスバー)
      int barHeight = map(potValues[i], 0, 1023, 0, 42); 
      u8g2.drawFrame(x, 22, 10, 42); // 外枠(最大高さ42)
      u8g2.drawBox(x, 22 + (42 - barHeight), 10, barHeight); // 塗りつぶしが下から上に伸びる
    }

    u8g2.sendBuffer(); 
  }
}
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <SerialFlash.h>
#include <U8g2lib.h> 

// --- GUItool: 5オシレーター和音構成 ---
AudioSynthNoiseWhite     noise1;
AudioSynthWaveform       wave1; // OSC 1 (Root)
AudioSynthWaveform       wave2; // OSC 2 (3rd)
AudioSynthWaveform       wave3; // OSC 3 (5th)
AudioSynthWaveform       wave4; // OSC 4 (Octave)
AudioSynthWaveform       wave5; // OSC 5 (Octave + 5th)

AudioMixer4              mixer1; // wave1〜4をまとめる
AudioMixer4              mixer2; // mixer1, wave5, noise をまとめる
AudioEffectEnvelope      envelope1;
AudioFilterLadder        ladder1;
AudioEffectBitcrusher    bitcrusher1;
AudioEffectFreeverb      freeverb1;
AudioOutputUSB           usb1;

// 内部配線
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

// --- OLEDディスプレイ設定 ---
U8G2_SSD1309_128X64_NONAME0_F_4W_HW_SPI u8g2(U8G2_R0, 10, 9, 8);

// --- ピン定義 ---
const int MUX_SIG = A10; 
const int MUX_S0 = 2;
const int MUX_S1 = 3;
const int MUX_S2 = 4;
const int MUX_S3 = 5;
const int POT_PINS[5] = {A0, A1, A2, A3, A4};
const int BTN_PINS[5] = {33, 34, 35, 36, 37};

// --- UIの状態変数 ---
int potValues[5] = {0};
bool prevBtnStates[5] = {false};

// ★ 波形モードのトラッキング (0:OFF, 1:SINE, 2:SQUARE, 3:SAW, 4:TRIANGLE)
int oscModes[5] = {0, 0, 0, 0, 0}; 

// 和音の周波数比率 (Root, 長3度, 完全5度, 1オクターブ, 1オクターブ上の5度)
float oscRatios[5] = {1.0, 1.25, 1.5, 2.0, 3.0}; 
AudioSynthWaveform* waves[5] = {&wave1, &wave2, &wave3, &wave4, &wave5};

// --- センサー解析用変数 ---
// ★ C0〜C5まで読み取るために「6」に変更しました
const int NUM_CHANNELS = 6; 
const int BUFFER_SIZE = 16; 
int buffers[NUM_CHANNELS][BUFFER_SIZE];
int bufferIndex = 0;

unsigned long lastStabTime = 0;
const int COOLDOWN_MS = 100;
bool isTouching = false;
unsigned long lastActiveTime = 0;

unsigned long lastDisplayUpdate = 0;
const int DISPLAY_INTERVAL = 80;

void setup() {
  Serial.begin(115200);
  AudioMemory(30); 
  u8g2.begin();

  pinMode(MUX_SIG, INPUT);
  pinMode(MUX_S0, OUTPUT);
  pinMode(MUX_S1, OUTPUT);
  pinMode(MUX_S2, OUTPUT);
  pinMode(MUX_S3, OUTPUT);

  for (int i = 0; i < 5; i++) {
    pinMode(BTN_PINS[i], INPUT_PULLUP); 
    waves[i]->amplitude(0.0); // 初期状態は無音(OFF)
  }

  noise1.amplitude(1.0); 
  
  // ミキサーの音量設定
  for(int i=0; i<4; i++) mixer1.gain(i, 1.0);
  mixer2.gain(0, 1.0); // OSC 1〜4
  mixer2.gain(1, 1.0); // OSC 5
  mixer2.gain(2, 0.0); // ノイズ

  envelope1.attack(100);
  envelope1.hold(10);
  envelope1.decay(300);
  envelope1.sustain(0.8);
  envelope1.release(1000);

  ladder1.frequency(2000);
  ladder1.resonance(0.3);
  bitcrusher1.bits(16);
  bitcrusher1.sampleRate(44100);
  freeverb1.roomsize(0.85);
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
  int activeOscCount = 0; // 現在ONになっているオシレーターの数

  // ==========================================
  // 1. UI（ボタンとツマミ）の読み取り
  // ==========================================
  for(int i = 0; i < 5; i++) {
    potValues[i] = analogRead(POT_PINS[i]);
    bool isPressed = (digitalRead(BTN_PINS[i]) == LOW);
    
    // ボタンが押された瞬間、波形モードを切り替える
    if(isPressed && !prevBtnStates[i]) {
      oscModes[i] = (oscModes[i] + 1) % 5; // 0, 1, 2, 3, 4 のループ
      
      // モードに合わせて波形を適用
      if(oscModes[i] == 1) waves[i]->begin(WAVEFORM_SINE);
      else if(oscModes[i] == 2) waves[i]->begin(WAVEFORM_SQUARE);
      else if(oscModes[i] == 3) waves[i]->begin(WAVEFORM_SAWTOOTH);
      else if(oscModes[i] == 4) waves[i]->begin(WAVEFORM_TRIANGLE);
    }
    prevBtnStates[i] = isPressed;

    if(oscModes[i] > 0) activeOscCount++;
  }

  // ツマミの値をシンセに反映
  freeverb1.roomsize(potValues[0] / 1023.0); 
  float resValue = potValues[1] / 1023.0;    
  envelope1.release(map(potValues[2], 0, 1023, 100, 5000)); 
  float noiseLevel = (potValues[3] / 1023.0) * 0.8; 
  float baseFreq = map(potValues[4], 0, 1023, 50, 880); 

  // 各オシレーターの周波数と音量を更新
  for(int i = 0; i < 5; i++) {
    waves[i]->frequency(baseFreq * oscRatios[i]);
    // モードが0(OFF)なら音量0、それ以外なら音量0.15
    waves[i]->amplitude((oscModes[i] > 0) ? 0.15 : 0.0); 
  }

  // ==========================================
  // 2. ピエゾセンサーのスキャン
  // ==========================================
  int maxDelta = 0; 
  for (int ch = 0; ch < NUM_CHANNELS; ch++) {
    selectMuxChannel(ch); 
    int rawValue = analogRead(MUX_SIG);
    buffers[ch][bufferIndex] = rawValue;
    
    int minVal = 1023;
    int maxVal = 0;
    for (int i = 0; i < BUFFER_SIZE; i++) {
      if (buffers[ch][i] > maxVal) maxVal = buffers[ch][i];
      if (buffers[ch][i] < minVal) minVal = buffers[ch][i];
    }
    int delta = maxVal - minVal;
    if (delta > maxDelta) maxDelta = delta; 
  }
  bufferIndex = (bufferIndex + 1) % BUFFER_SIZE;

  // ==========================================
  // 3. 状態遷移ロジック
  // ==========================================
  if (maxDelta > 800) { 
    ladder1.frequency(12000); 
    mixer2.gain(2, 1.0); 
    if(activeOscCount == 0) envelope1.noteOn(); 
    lastStabTime = currentTime;
    lastActiveTime = currentTime;
    isTouching = true;
  }
  else if (maxDelta > 150 && (currentTime - lastStabTime > COOLDOWN_MS)) {
    ladder1.frequency(8000);
    mixer2.gain(2, noiseLevel + 0.5); 
    if(activeOscCount == 0) envelope1.noteOn();
    lastStabTime = currentTime;
    lastActiveTime = currentTime;
    isTouching = true;
  }
  else if (maxDelta > 15) {
    if(activeOscCount == 0 && !isTouching) { envelope1.noteOn(); isTouching = true; } 
    lastActiveTime = currentTime;
    
    float cutoff = map(maxDelta, 5, 30, baseFreq * 2, 8000);
    if (cutoff > 8000) cutoff = 8000;
    ladder1.frequency(cutoff);
    ladder1.resonance(resValue); 
    mixer2.gain(2, noiseLevel * 0.5); 
  }
  else {
    if (activeOscCount > 0) {
      if (!isTouching) { envelope1.noteOn(); isTouching = true; }
      ladder1.frequency(baseFreq * 3); 
      ladder1.resonance(resValue); 
      mixer2.gain(2, noiseLevel * 0.1); 
      lastActiveTime = currentTime;
    } else {
      if (isTouching && (currentTime - lastActiveTime > 200)) {
        envelope1.noteOff(); 
        isTouching = false;
      }
    }
  }

  // ==========================================
  // 4. OLEDディスプレイの更新
  // ==========================================
  if (currentTime - lastDisplayUpdate > DISPLAY_INTERVAL) {
    lastDisplayUpdate = currentTime;
    
    u8g2.clearBuffer(); 
    u8g2.setFont(u8g2_font_5x7_tr); // はみ出さない小さなフォント

    // --- 上段：各オシレーターの波形状態 ---
    u8g2.setCursor(0, 8);
    u8g2.print("OSC:");
    for(int i = 0; i < 5; i++) {
      int x = 23 + (i * 21); // 横に21ピクセル刻みで均等配置
      if(oscModes[i] > 0) {
        u8g2.drawBox(x, 0, 19, 10); // 背景を白塗り
        u8g2.setDrawColor(0); // 文字を黒に反転
        u8g2.setCursor(x + 2, 8);
        if(oscModes[i] == 1) u8g2.print("SIN");
        else if(oscModes[i] == 2) u8g2.print("SQR");
        else if(oscModes[i] == 3) u8g2.print("SAW");
        else if(oscModes[i] == 4) u8g2.print("TRI");
        u8g2.setDrawColor(1); // 白に戻す
      } else {
        u8g2.drawFrame(x, 0, 19, 10); // 枠線のみ
        u8g2.setCursor(x + 3, 8);
        u8g2.print("---"); // OFF状態
      }
    }

    // --- 下段：ツマミの値を横棒グラフで表示 ---
    const char* labels[] = {"Rvb", "Res", "Rel", "Noi", "Frq"};
    for(int i = 0; i < 5; i++) {
      int y = 20 + (i * 10); // 10ピクセル刻み（見切れ防止）
      u8g2.setCursor(0, y);
      u8g2.print(labels[i]); 
      
      int barWidth = map(potValues[i], 0, 1023, 0, 90); 
      u8g2.drawFrame(25, y - 6, 90, 6); 
      u8g2.drawBox(25, y - 6, barWidth, 6); 
    }

    u8g2.sendBuffer(); 
  }
}
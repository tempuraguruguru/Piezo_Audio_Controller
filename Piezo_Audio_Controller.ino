#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <SerialFlash.h>
#include <U8g2lib.h>

// ==========================================
// 衝撃の閾値（ピエゾ感度の調整エリア）
// ==========================================
const int THRESHOLD_TOUCH = 15;  // 触る
const int THRESHOLD_TRACE = 30;  // なぞる
const int THRESHOLD_STAB  = 150; // 突き刺す
const int THRESHOLD_TEAR  = 500; // 破る
const int THRESHOLD_CRUSH = 900; // 壊す

// ==========================================
// 音響モジュール・ルーティング設定
// ==========================================
AudioSynthNoiseWhite     noise1;
AudioSynthWaveform       waveMain;
AudioSynthWaveform       waveSub;
AudioSynthWaveformModulated waveLFO;

AudioMixer4              oscMixer; 
AudioMixer4              mainMixer; 
AudioEffectEnvelope      envelope1;
AudioFilterLadder        ladder1;
AudioEffectBitcrusher    bitcrusher1;
AudioEffectDelay         delay1;
AudioMixer4              delayMixer;
AudioEffectFreeverb      freeverb1;
AudioOutputUSB           usb1;

// パッチコード（配線）
AudioConnection          patchCord1(waveMain, 0, oscMixer, 0);
AudioConnection          patchCord2(waveSub, 0, oscMixer, 1);
AudioConnection          patchCord3(oscMixer, 0, bitcrusher1, 0);
AudioConnection          patchCord4(bitcrusher1, 0, ladder1, 0);
AudioConnection          patchCord5(ladder1, 0, mainMixer, 0);
AudioConnection          patchCord6(noise1, 0, mainMixer, 1); // 衝撃時のノイズ入力
AudioConnection          patchCord7(mainMixer, envelope1);
AudioConnection          patchCord8(envelope1, 0, delayMixer, 0);
AudioConnection          patchCord9(delayMixer, 0, delay1, 0);
AudioConnection          patchCord10(delay1, 0, delayMixer, 1);
AudioConnection          patchCord11(delayMixer, freeverb1);
AudioConnection          patchCord12(freeverb1, 0, usb1, 0);
AudioConnection          patchCord13(freeverb1, 0, usb1, 1);

// ==========================================
// ハードウェアピン設定
// ==========================================
U8G2_SSD1309_128X64_NONAME0_F_4W_HW_SPI u8g2(U8G2_R0, 10, 9, 8);

const int MUX_SIG = A10; 
const int MUX_S0 = 2; const int MUX_S1 = 3; 
const int MUX_S2 = 4; const int MUX_S3 = 5;

const int POT_PINS[10] = {A0, A1, A2, A3, A4, A5, A6, A7, A8, A9};
const int BTN_PINS[10] = {28, 29, 30, 31, 32, 33, 34, 35, 36, 37};

// ==========================================
// システム状態とパラメータ
// ==========================================
enum SystemMode { MODE_RESET, MODE_EDIT, MODE_LOCKED, MODE_SOMEONE };
SystemMode currentMode = MODE_EDIT;
String displayMessage = "設定モード"; 
unsigned long messageTimer = 0;

float params[10] = {0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5};
bool prevBtnStates[10] = {false};

const int NUM_CHANNELS = 6; 
const int BUFFER_SIZE = 16; 
int buffers[NUM_CHANNELS][BUFFER_SIZE];
int bufferIndex = 0;

unsigned long lastActiveTime = 0;
unsigned long lastDisplayUpdate = 0;
bool isTouching = false;
int fakeTriggerForce = 0; 

// ==========================================
// SDカード関連関数
// ==========================================
void loadParamsFromSD(const char* filename) {
  File file = SD.open(filename);
  if (file) {
    for (int i = 0; i < 10; i++) {
      if (file.available()) {
        String line = file.readStringUntil('\n');
        params[i] = line.toFloat();
      }
    }
    file.close();
    String dispName = String(filename);
    dispName.replace(".TXT", "");
    dispName.replace(".txt", "");
    dispName.toUpperCase();
    displayMessage = "読込: " + dispName;
  } else {
    displayMessage = "ファイルなし";
  }
  messageTimer = millis();
}

void saveAsNewUserToSD() {
  int userNumber = 1;
  String filename;
  
  while (true) {
    filename = "user" + String(userNumber) + ".txt";
    if (!SD.exists(filename.c_str())) {
      break; 
    }
    userNumber++;
  }
  
  File file = SD.open(filename.c_str(), FILE_WRITE);
  if (file) {
    for (int i = 0; i < 10; i++) {
      file.println(params[i], 4);
    }
    file.close();
    displayMessage = "保存: 記録" + String(userNumber); 
  } else {
    displayMessage = "保存エラー";
  }
  messageTimer = millis();
}

void loadRandomSomeoneFromSD() {
  File root = SD.open("/");
  if (!root) {
    displayMessage = "SDエラー";
    messageTimer = millis();
    return;
  }
  
  int fileCount = 0;
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break; 
    if (!entry.isDirectory()) {
      String fname = String(entry.name());
      fname.toLowerCase();
      if (fname.endsWith(".txt") && fname != "nomoto.txt") {
        fileCount++;
      }
    }
    entry.close();
  }
  
  if (fileCount == 0) {
    displayMessage = "他者がいません";
    messageTimer = millis();
    root.close();
    return;
  }
  
  randomSeed(micros()); 
  int randomIndex = random(0, fileCount);
  
  root.rewindDirectory();
  int currentIndex = 0;
  String selectedFilename = "";
  
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    if (!entry.isDirectory()) {
      String fname = String(entry.name());
      String fnameLower = fname;
      fnameLower.toLowerCase();
      if (fnameLower.endsWith(".txt") && fnameLower != "nomoto.txt") {
        if (currentIndex == randomIndex) {
          selectedFilename = fname; 
          entry.close();
          break;
        }
        currentIndex++;
      }
    }
    entry.close();
  }
  root.close();
  
  if (selectedFilename != "") {
    loadParamsFromSD(selectedFilename.c_str());
  }
}

// ==========================================
// 起動時の初期設定
// ==========================================
void setup() {
  Serial.begin(115200);
  AudioMemory(40);
  u8g2.begin();
  u8g2.enableUTF8Print(); // 日本語表示の許可

  if (!SD.begin(BUILTIN_SDCARD)) {
    displayMessage = "SD初期化失敗";
  }

  pinMode(MUX_SIG, INPUT);
  pinMode(MUX_S0, OUTPUT); pinMode(MUX_S1, OUTPUT);
  pinMode(MUX_S2, OUTPUT); pinMode(MUX_S3, OUTPUT);

  for (int i = 0; i < 10; i++) { pinMode(BTN_PINS[i], INPUT_PULLUP); }

  waveMain.begin(WAVEFORM_SINE);
  waveSub.begin(WAVEFORM_SINE);
  noise1.amplitude(1.0); // 衝撃時のノイズ源
  
  oscMixer.gain(0, 1.0); oscMixer.gain(1, 0.0);
  mainMixer.gain(0, 1.0); mainMixer.gain(1, 0.0);
  delayMixer.gain(0, 1.0); delayMixer.gain(1, 0.0);

  ladder1.frequency(2000);
  ladder1.resonance(0.1);
  bitcrusher1.bits(16);
  bitcrusher1.sampleRate(44100);
  freeverb1.roomsize(0.5);
}

void selectMuxChannel(int channel) {
  digitalWrite(MUX_S0, bitRead(channel, 0));
  digitalWrite(MUX_S1, bitRead(channel, 1));
  digitalWrite(MUX_S2, bitRead(channel, 2));
  digitalWrite(MUX_S3, bitRead(channel, 3));
  delayMicroseconds(5); 
}

// ==========================================
// メインループ
// ==========================================
void loop() {
  unsigned long currentTime = millis();
  fakeTriggerForce = 0;

  // --- 1. スイッチ入力（モード遷移） ---
  for(int i = 0; i < 10; i++) {
    bool isPressed = (digitalRead(BTN_PINS[i]) == LOW);
    if(isPressed && !prevBtnStates[i]) {
      switch(i) {
        case 0: 
          currentMode = MODE_RESET;
          loadParamsFromSD("nomoto.txt");
          break;
        case 1: 
          currentMode = MODE_EDIT;
          displayMessage = "設定モード"; messageTimer = currentTime;
          break;
        case 2: fakeTriggerForce = THRESHOLD_TOUCH; displayMessage = "触れる"; messageTimer = currentTime; break;
        case 3: fakeTriggerForce = THRESHOLD_TRACE; displayMessage = "なぞる"; messageTimer = currentTime; break;
        case 4: fakeTriggerForce = THRESHOLD_STAB;  displayMessage = "突き刺す";  messageTimer = currentTime; break;
        case 5: fakeTriggerForce = THRESHOLD_TEAR;  displayMessage = "破る";  messageTimer = currentTime; break;
        case 6: fakeTriggerForce = THRESHOLD_CRUSH; displayMessage = "壊す"; messageTimer = currentTime; break;
        case 7: 
          currentMode = MODE_LOCKED;
          displayMessage = "固定しました"; messageTimer = currentTime;
          break;
        case 8: 
          saveAsNewUserToSD(); 
          break;
        case 9: 
          currentMode = MODE_SOMEONE;
          loadRandomSomeoneFromSD(); 
          break;
      }
    }
    prevBtnStates[i] = isPressed;
  }

  // --- 2. ツマミ入力（設定モード時のみ反映） ---
  if (currentMode == MODE_EDIT) {
    for(int i = 0; i < 10; i++) {
      params[i] = analogRead(POT_PINS[i]) / 1023.0; 
    }
  }

  // --- 3. パラメータのベース音響マッピング ---
  bitcrusher1.bits(map(params[0], 0.0, 1.0, 16, 3)); 
  freeverb1.roomsize(params[1]); 
  
  float baseResonance = map(params[2], 0.0, 1.0, 0.1, 0.9); 
  float baseCutoff = map(params[6], 0.0, 1.0, 500, 4000); 
  
  oscMixer.gain(1, params[3]); 
  float wobble = sin(currentTime / 200.0) * (params[4] * 50.0);
  delayMixer.gain(1, params[5] * 0.8); 
  
  envelope1.attack(map(params[7], 0.0, 1.0, 5, 2000));
  float vitality = map(params[8], 0.0, 1.0, 0.2, 1.0);
  float detune = map(params[9], 0.0, 1.0, 1.0, 1.05);

  float basePitch = 220.0 + wobble;
  waveMain.frequency(basePitch);
  waveSub.frequency((basePitch / 2.0) * detune);
  waveMain.amplitude(vitality);
  waveSub.amplitude(vitality);

  // --- 4. ピエゾセンサースキャンと衝撃処理 ---
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

  int finalImpact = max(maxDelta, fakeTriggerForce); 

  if (finalImpact >= THRESHOLD_TOUCH) {
    float impactNoise = 0.0;
    float impactCutoffAdd = 0.0;
    float impactResAdd = 0.0;

    if (finalImpact >= THRESHOLD_CRUSH) {
      impactNoise = 1.0; 
      impactCutoffAdd = 8000;
      impactResAdd = 0.5;
    } 
    else if (finalImpact >= THRESHOLD_TEAR) {
      impactNoise = 0.4;
      impactCutoffAdd = 4000;
      impactResAdd = 0.2;
    }
    else if (finalImpact >= THRESHOLD_STAB) {
      impactNoise = 0.0;
      impactCutoffAdd = 6000;
      impactResAdd = 0.3;
    }
    else if (finalImpact >= THRESHOLD_TRACE) {
      impactNoise = 0.05;
      impactCutoffAdd = 1000;
      impactResAdd = 0.0;
    }
    else {
      impactNoise = 0.0;
      impactCutoffAdd = 200;
      impactResAdd = 0.0;
    }

    mainMixer.gain(1, impactNoise); 
    ladder1.frequency(baseCutoff + impactCutoffAdd); 
    ladder1.resonance(min(1.0, baseResonance + impactResAdd)); 

    if (!isTouching) {
      envelope1.noteOn(); 
      isTouching = true;
    }
    lastActiveTime = currentTime;
    
  } else {
    // 触っていない時（減衰中）
    mainMixer.gain(1, 0.0); 
    ladder1.frequency(baseCutoff); 
    ladder1.resonance(baseResonance); 

    if (isTouching && (currentTime - lastActiveTime > 300)) {
      envelope1.noteOff(); 
      isTouching = false;
    }
  }

  // --- 5. OLEDディスプレイのモード表示 ---
  if (currentTime - lastDisplayUpdate > 100) {
    lastDisplayUpdate = currentTime;
    
    u8g2.clearBuffer(); 
    // ※文字化けする場合は u8g2_font_unifont_t_japanese3 に変更してください
    u8g2.setFont(u8g2_font_b14_t_japanese2); 

    int textWidth = u8g2.getStrWidth(displayMessage.c_str());
    u8g2.setCursor((128 - textWidth) / 2, 38);
    u8g2.print(displayMessage);

    if (currentTime - messageTimer > 2000) {
      if (currentMode == MODE_EDIT) displayMessage = "設定モード";
      else if (currentMode == MODE_LOCKED) displayMessage = "固定しました";
      else if (currentMode == MODE_RESET) displayMessage = "[ 私 ]";
      else if (currentMode == MODE_SOMEONE) displayMessage = "[ 誰か ]";
    }
    u8g2.sendBuffer(); 
  }
}
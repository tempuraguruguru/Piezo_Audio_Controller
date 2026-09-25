#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <SerialFlash.h>

// --- GUItool生成コード（完全一致） ---
AudioSynthNoiseWhite     noise1;
AudioSynthWaveform       waveform1;
AudioMixer4              mixer1;
AudioEffectEnvelope      envelope1;
AudioFilterLadder        ladder1;
AudioEffectBitcrusher    bitcrusher1;
AudioEffectFreeverb      freeverb1;
AudioOutputUSB           usb1;
AudioConnection          patchCord1(noise1, 0, mixer1, 1);
AudioConnection          patchCord2(waveform1, 0, mixer1, 0);
AudioConnection          patchCord3(mixer1, envelope1);
AudioConnection          patchCord4(envelope1, 0, ladder1, 0);
AudioConnection          patchCord5(ladder1, bitcrusher1);
AudioConnection          patchCord6(bitcrusher1, freeverb1);
AudioConnection          patchCord7(freeverb1, 0, usb1, 0);
AudioConnection          patchCord8(freeverb1, 1, usb1, 1);

// --- センサー解析用変数 ---
const int PIEZO_PIN = A0;
const int BUFFER_SIZE = 32;
int buffer[BUFFER_SIZE];
int bufferIndex = 0;

unsigned long lastStabTime = 0;
const int COOLDOWN_MS = 100;
bool isTouching = false; // 「触れている状態」のトラッキング

// 最後のセンサー反応時間を記録する変数
unsigned long lastActiveTime = 0;

void setup() {
  Serial.begin(115200);
  AudioMemory(20); // オーディオ処理用のメモリ確保（必須）
  pinMode(PIEZO_PIN, INPUT);

  // --- シンセサイザーの初期パラメーター ---
  // ★キラキラ感を出すため、純粋な「サイン波」に変更
  waveform1.begin(WAVEFORM_SINE); 
  waveform1.amplitude(0.5);

  noise1.amplitude(1.0); 
  mixer1.gain(0, 1.0); // Waveformの音量
  mixer1.gain(1, 0.0); // 初期状態ではNoiseをミュート

  // ★空間系の要：フワッと立ち上がり、長く残るアンビエントな設定
  envelope1.attack(150);    // 少しフワッと優しく立ち上がる
  envelope1.hold(10);
  envelope1.decay(300);
  envelope1.sustain(0.6);
  envelope1.release(3000);  // ★触るのをやめた後、3秒間かけて美しく消える

  ladder1.frequency(2000);
  ladder1.resonance(0.3);
  
  bitcrusher1.bits(16);
  bitcrusher1.sampleRate(44100);

  // ★リバーブ（空間）の設定
  freeverb1.roomsize(0.85); // 0.0〜1.0 (部屋の広さ。大きいほど大聖堂のような大空間に)
  freeverb1.damping(0.2);   // 0.0〜1.0 (高音の吸収率。低いほど高音がキラキラと反射する)
}

void loop() {
  // 1. バッファの更新
  int rawValue = analogRead(PIEZO_PIN);
  buffer[bufferIndex] = rawValue;
  
  // 2. 波形のピーク抽出
  int minVal = 1023;
  int maxVal = 0;
  for (int i = 0; i < BUFFER_SIZE; i++) {
    if (buffer[i] > maxVal) maxVal = buffer[i];
    if (buffer[i] < minVal) minVal = buffer[i];
  }
  
  int delta = maxVal - minVal;
  int prevIndex = (bufferIndex - 1 + BUFFER_SIZE) % BUFFER_SIZE;
  int instantDelta = abs(rawValue - buffer[prevIndex]);
  unsigned long currentTime = millis();

  // ==== 【閾値キャリブレーション用】 ====
  // プロッターに「実際の波形」と「閾値のボーダーライン」を同時に描画する
  Serial.print("Delta:");
  Serial.print(delta);
  Serial.print(",");
  Serial.print("Touch:");
  Serial.print(15);
  Serial.print(",");
  Serial.print("Trace:");
  Serial.print(5);
  Serial.print(",");
  Serial.print("Stab:");
  Serial.print(150);
  Serial.print(",");
  Serial.print("Crush:");
  Serial.print(800);
  Serial.print(",");
  Serial.print("MAX:");
  Serial.println(1023); // ここだけ println（改行）にする
  // =================================

  // --- 3. 4つの状態遷移ロジック ---

  // 【壊す】: 空間が歪むようなグリッチノイズ
  if (delta > 800) {
    bitcrusher1.bits(4); 
    ladder1.frequency(12000); 
    mixer1.gain(1, 0.8); 
    envelope1.noteOn();
    lastStabTime = currentTime;
    lastActiveTime = currentTime;
    isTouching = true;
  }
  // 【突き刺す】: ランダムな高音のキラキラしたチャイム音
  else if (instantDelta > 150 && (currentTime - lastStabTime > COOLDOWN_MS)) {
    bitcrusher1.bits(16); 
    waveform1.frequency(random(880, 3520)); // ★高音域（ソプラノ）へランダムに飛ぶ
    mixer1.gain(1, 0.0); // 突き刺す時は純粋なサイン波のみにする
    envelope1.noteOn(); 
    lastStabTime = currentTime;
    lastActiveTime = currentTime;
    isTouching = true;
  }
  // 【なぞる】: サァーッという風のような広がり
  else if (delta > 5) {
    bitcrusher1.bits(16);
    if (!isTouching) { envelope1.noteOn(); isTouching = true; } 
    lastActiveTime = currentTime;
    
    // 摩擦の音を高音域のフィルターでシュワシュワさせる
    float cutoff = map(delta, 5, 30, 1000, 8000);
    if (cutoff > 8000) cutoff = 8000;
    if (cutoff < 1000) cutoff = 1000;

    ladder1.frequency(cutoff);
    ladder1.resonance(0.6); 
    mixer1.gain(1, 0.3); // 風のようなノイズを混ぜる
  }
  // 【触る】: 澄んだクリスタルのような持続音
  else if (delta > 2) {
    bitcrusher1.bits(16);
    if (!isTouching) { envelope1.noteOn(); isTouching = true; }
    lastActiveTime = currentTime;

    waveform1.frequency(880.0); // ★基本の音を高めの澄んだ音（A5）に設定
    ladder1.frequency(3000); 
    ladder1.resonance(0.1);
    mixer1.gain(1, 0.05); // ノイズはほぼ無し
  }
  // 【沈黙】
  else {
    if (isTouching && (currentTime - lastActiveTime > 2000)) {
      envelope1.noteOff(); 
      isTouching = false;
    }
  }

  bufferIndex = (bufferIndex + 1) % BUFFER_SIZE;
  delay(2);
}
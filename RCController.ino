#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <ESP32Servo.h>
#include <U8g2lib.h>
#include <Wire.h>

// ==================================================
// GPIO設定
// ==================================================

// Mini L298N (ENAなし。IN1/IN2に直接PWMを出力する)
#define MOTOR_IN1 3
#define MOTOR_IN2 4

// Steering Servo
#define SERVO_PIN 7

// OLED I2C
#define OLED_SDA 5
#define OLED_SCL 6


// ==================================================
// BLE UUID
// 送信機(リレー)側と一致させる
// ==================================================

#define SERVICE_UUID \
  "12345678-1234-5678-1234-56789abcdef0"

#define CHARACTERISTIC_UUID \
  "12345678-1234-5678-1234-56789abcdef1"


// ==================================================
// ユーザー設定
// ==================================================

// ステアリング最大角度 (中央から左右への角度)
const int STEERING_MAX_ANGLE = 25;

// サーボ中央角度
const int STEERING_CENTER = 90;

// ステアリング方向反転
const bool STEERING_REVERSE = false;

// モーター方向反転
const bool MOTOR_REVERSE = false;

// モーター最低操作値 (これ以下は停止)
const int MOTOR_DEADZONE = 8;

// モーターが回り始める最低PWM値 (0〜255)
// 低速で反応しない/唸るだけの場合は大きく、飛び出しが強い場合は小さくする
const int MOTOR_MIN_PWM = 50;

// 停止方法: true=ブレーキ(IN1,IN2とも最大), false=フリー(IN1,IN2とも0)
const bool MOTOR_BRAKE_ON_STOP = true;

// BLE通信途絶タイムアウト(ms)
const unsigned long BLE_TIMEOUT = 200;


// ==================================================
// PWM設定
// ==================================================

// L298系は高い周波数だと低デューティで駆動力が出ず、反応が鈍くなる。
// 唸り音が気になる場合は 10000 に、反応が悪い場合は 2000〜3000 に。
const int PWM_FREQ = 4000;
const int PWM_RESOLUTION = 8;
//const int PWM_MAX = 255;
const int PWM_MAX = 198;

// ==================================================
// OLED
// 72 x 40 SSD1306 ER
// ==================================================

U8G2_SSD1306_72X40_ER_F_HW_I2C
u8g2(U8G2_R0, U8X8_PIN_NONE);


// ==================================================
// サーボ
// ==================================================

Servo steeringServo;


// ==================================================
// 状態
// ==================================================

volatile unsigned long lastReceiveMs = 0;

volatile bool bleConnected = false;
volatile bool commandValid = false;

// 制御値 (表示用)
volatile int currentLR = 0;
volatile int currentFB = 0;

volatile int currentMotorPWM = 0;
volatile int currentServoAngle = STEERING_CENTER;

bool failsafe = true;

// BLEタスクとloop()からの同時アクセス防止
SemaphoreHandle_t ctrlMutex;


// ==================================================
// モーター初期化
// IN1/IN2の両方をPWM出力にする (Arduino-ESP32 Core 3.x)
// ==================================================

void setupMotor() {

  ledcAttach(MOTOR_IN1, PWM_FREQ, PWM_RESOLUTION);
  ledcAttach(MOTOR_IN2, PWM_FREQ, PWM_RESOLUTION);

  ledcWrite(MOTOR_IN1, 0);
  ledcWrite(MOTOR_IN2, 0);
}


// ==================================================
// モーター制御
// FB -100 ～ +100
//   前進: IN1=PWM, IN2=0
//   後退: IN1=0,   IN2=PWM
// ※ 呼び出し側でctrlMutexを取得すること
// ==================================================

void controlMotor(int fb, bool brake) {

  if (MOTOR_REVERSE) {
    fb = -fb;
  }

  // 停止
  if (abs(fb) <= MOTOR_DEADZONE) {

    int stopLevel = brake ? PWM_MAX : 0;

    ledcWrite(MOTOR_IN1, stopLevel);
    ledcWrite(MOTOR_IN2, stopLevel);

    currentMotorPWM = 0;

    return;
  }

  // 速度 → PWM変換 (MOTOR_MIN_PWM〜PWM_MAX)
  int pwm = map(
    abs(fb),
    MOTOR_DEADZONE,
    100,
    MOTOR_MIN_PWM,
    PWM_MAX);

  pwm = constrain(pwm, 0, PWM_MAX);

  if (fb > 0) {
    // 前進
    ledcWrite(MOTOR_IN2, 0);
    ledcWrite(MOTOR_IN1, pwm);
    currentMotorPWM = pwm;
  } else {
    // 後退
    ledcWrite(MOTOR_IN1, 0);
    ledcWrite(MOTOR_IN2, pwm);
    currentMotorPWM = -pwm;
  }
}


// ==================================================
// ステアリング制御
// LR -100 ～ +100
// ※ 呼び出し側でctrlMutexを取得すること
// ==================================================

void controlSteering(int lr) {

  if (STEERING_REVERSE) {
    lr = -lr;
  }

  int angle = STEERING_CENTER + (lr * STEERING_MAX_ANGLE / 100);

  angle = constrain(angle, 0, 180);

  steeringServo.write(angle);

  currentServoAngle = angle;
}


// ==================================================
// 指令の即時反映 (BLE受信コールバックから直接呼ぶ)
// loop()やOLED描画を待たずにモーター/サーボへ反映する
// ==================================================

void applyCommand(int lr, int fb) {

  xSemaphoreTake(ctrlMutex, portMAX_DELAY);

  currentLR = lr;
  currentFB = fb;

  controlMotor(fb, MOTOR_BRAKE_ON_STOP);
  controlSteering(lr);

  xSemaphoreGive(ctrlMutex);
}


// ==================================================
// フェイルセーフ (常にブレーキで停止し、サーボは中央)
// ==================================================

void emergencyStop() {

  xSemaphoreTake(ctrlMutex, portMAX_DELAY);

  currentFB = 0;
  currentLR = 0;

  controlMotor(0, true);
  controlSteering(0);

  xSemaphoreGive(ctrlMutex);
}


// ==================================================
// BLEコールバック
// ==================================================

class ServerCallbacks : public BLEServerCallbacks {

  void onConnect(BLEServer* server) override {

    bleConnected = true;

    Serial.println("BLE Connected");
  }

  void onDisconnect(BLEServer* server) override {

    bleConnected = false;
    commandValid = false;

    Serial.println("BLE Disconnected");

    // 再接続可能にする
    BLEDevice::startAdvertising();
  }
};


class CharacteristicCallbacks : public BLECharacteristicCallbacks {

  void onWrite(BLECharacteristic* characteristic) override {

    String value = characteristic->getValue();

    // 2バイト未満は無効データとして破棄
    if (value.length() < 2) {
      return;
    }

    int8_t lr = static_cast<int8_t>(value[0]);
    int8_t fb = static_cast<int8_t>(value[1]);

    // -100～100の範囲に制限
    lr = constrain(lr, -100, 100);
    fb = constrain(fb, -100, 100);

    // 先に有効フラグを立て、loop()のフェイルセーフに上書きされないようにする
    lastReceiveMs = millis();
    commandValid = true;

    // 受信した瞬間にモーター/サーボへ反映
    applyCommand(lr, fb);
  }
};


// ==================================================
// BLE初期化
// ==================================================

void setupBLE() {

  BLEDevice::init("ESP32C3-RC");

  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService* service = server->createService(SERVICE_UUID);

  BLECharacteristic* characteristic =
    service->createCharacteristic(
      CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);

  characteristic->setCallbacks(new CharacteristicCallbacks());

  service->start();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);

  BLEDevice::startAdvertising();

  Serial.println("BLE Advertising Started");
}


// ==================================================
// OLED表示
// I2C転送は時間がかかるので、制御とは独立して低頻度で更新する
// ==================================================

void updateDisplay() {

  static unsigned long lastDisplayMs = 0;

  if (millis() - lastDisplayMs < 200) {
    return;
  }

  lastDisplayMs = millis();

  char line[24];

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_5x7_tf);

  // BLE状態
  if (bleConnected && !failsafe) {
    u8g2.drawStr(0, 8, "BLE:OK");
  } else if (bleConnected) {
    u8g2.drawStr(0, 8, "BLE:WAIT");
  } else {
    u8g2.drawStr(0, 8, "BLE:OFF");
  }

  snprintf(line, sizeof(line), "FB:%+4d", (int)currentFB);
  u8g2.drawStr(0, 18, line);

  snprintf(line, sizeof(line), "LR:%+4d", (int)currentLR);
  u8g2.drawStr(0, 28, line);

  // PWM(符号付き: +前進/-後退) / サーボ角度
  snprintf(line, sizeof(line), "M:%+4d S:%3d", (int)currentMotorPWM, (int)currentServoAngle);
  u8g2.drawStr(0, 38, line);

  u8g2.sendBuffer();
}


// ==================================================
// setup
// ==================================================

void setup() {

  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("ESP32-C3 RC Controller");

  ctrlMutex = xSemaphoreCreateMutex();

  // サーボを先に初期化 (LEDCタイマー競合の回避)
  ESP32PWM::allocateTimer(0);
  steeringServo.setPeriodHertz(50);
  steeringServo.attach(SERVO_PIN, 500, 2500);
  steeringServo.write(STEERING_CENTER);
  currentServoAngle = STEERING_CENTER;

  // モーター初期化
  setupMotor();

  // OLED初期化 (I2Cを400kHzにして描画の待ち時間を短縮)
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  u8g2.begin();
  u8g2.setBusClock(400000);

  // BLE初期化
  setupBLE();

  emergencyStop();

  Serial.println("Ready");
}


// ==================================================
// loop
// 制御はBLE受信時に即時実行されるため、ここでは
// 通信途絶の監視(フェイルセーフ)と表示のみ行う
// ==================================================

void loop() {

  static unsigned long lastStopMs = 0;

  unsigned long now = millis();

  bool fresh =
    bleConnected && commandValid && (now - lastReceiveMs < BLE_TIMEOUT);

  if (!fresh) {

    if (!failsafe) {

      // failsafeに入った瞬間だけログを出して停止
      Serial.printf(
        "FAILSAFE (connected:%d valid:%d age:%lums)\n",
        (int)bleConnected,
        (int)commandValid,
        now - lastReceiveMs);

      failsafe = true;
      emergencyStop();
      lastStopMs = now;

    } else if (now - lastStopMs >= 200) {

      // 念のため定期的に停止状態を再設定
      emergencyStop();
      lastStopMs = now;
    }

  } else {

    failsafe = false;
  }

  updateDisplay();

  delay(2);
}

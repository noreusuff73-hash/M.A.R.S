// ESP32 Mars comms: SOS buzzer + I2C 16x2 LCD + RGB status LED, controlled over BLE.
// Commands (each ends with a newline):
//   SOS | STOP | TEST
//   GOOD:<text>  -> 1 beep, GREEN   (affirmative, route safe, returning)
//   WARN:<text>  -> 2 beeps, AMBER  (low oxygen, need assistance)
//   MSG:<text>   -> 1 beep, BLUE    (custom message)
//   SOS          -> RED, Morse S-O-S on the buzzer
//
// Wiring (all 3.3 V)
//   Buzzer: 3V3 -> buzzer+, buzzer- -> transistor collector, GPIO 25 -> 1k -> base, emitter -> GND
//   LCD   : GND, VCC -> 3V3, SDA -> GPIO 21, SCL -> GPIO 22
//   RGB   : R -> GPIO 26 (220 ohm), G -> GPIO 27 (100 ohm), B -> GPIO 33 (100 ohm), common leg -> GND
// Library: "LiquidCrystal I2C" by Frank de Brabander.
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#define BUZ 25
#define LED 2
#define SDA_PIN 21
#define SCL_PIN 22
#define RGB_R 26
#define RGB_G 27
#define RGB_B 33
#define COMMON_ANODE false  // set true if your RGB LED's long leg goes to 3V3

#define NUS_SVC "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX  "6e400002-b5a3-f393-e0a9-e50e24dcca9e"

// SOS timing
const unsigned long DOT = 300;
const unsigned long DASH = 1000;
const unsigned long GAP = 250;
const unsigned long PAUSE = 2000;
const unsigned long MAX_TIME = 120000;

volatile bool sosOn = false;
volatile unsigned long sosStart = 0;
volatile bool testReq = false;
volatile bool msgReady = false;
String pendingMsg = "";
char pendingType = 'M';  // G = good, W = warning, M = custom
String lastMsg = "";
char lastType = 'M';
String rxBuf = "";
bool statR = false, statG = false, statB = false;  // status colour shown when SOS is off

enum State { IDLE, BEEPING, GAP_STATE, REPEAT_PAUSE };
State state = IDLE;
unsigned long phaseEnd = 0;
int pulseIndex = 0;
bool previousSOS = false;

LiquidCrystal_I2C *lcd = nullptr;
bool lcdOk = false;

// ---------- Outputs ----------
void setOutput(bool on) {  // buzzer only; the RGB LED is the indicator light
  digitalWrite(BUZ, on ? HIGH : LOW);
}

void rgb(bool r, bool g, bool b) {
  if (COMMON_ANODE) { r = !r; g = !g; b = !b; }
  digitalWrite(RGB_R, r);
  digitalWrite(RGB_G, g);
  digitalWrite(RGB_B, b);
}

void applyStatus() { rgb(statR, statG, statB); }

void setStatusFor(char t) {
  if (t == 'G') { statR = 0; statG = 1; statB = 0; }       // green
  else if (t == 'W') { statR = 1; statG = 1; statB = 0; }  // amber
  else { statR = 0; statG = 0; statB = 1; }                // blue
}

void blockingBeep(unsigned long ms) {
  setOutput(true);
  delay(ms);
  setOutput(false);
}

void notify(char t) {  // colour flashes only while each beep sounds, then the LED goes off
  setStatusFor(t);
  int n = (t == 'W') ? 2 : 1;  // warning = 2 beeps, everything else = 1
  for (int i = 0; i < n; i++) {
    applyStatus();
    blockingBeep(t == 'W' ? 150 : 200);
    rgb(0, 0, 0);
    if (i < n - 1) delay(120);
  }
}

// ---------- LCD ----------
void lcdInit() {
  Wire.begin(SDA_PIN, SCL_PIN);
  uint8_t addrs[2] = {0x27, 0x3F};
  for (int i = 0; i < 2; i++) {
    Wire.beginTransmission(addrs[i]);
    if (Wire.endTransmission() == 0) {
      lcd = new LiquidCrystal_I2C(addrs[i], 16, 2);
      lcd->init();
      lcd->backlight();
      lcdOk = true;
      return;
    }
  }
}

void lcdShow(String a, String b) {
  if (!lcdOk) return;
  lcd->clear();
  lcd->setCursor(0, 0);
  lcd->print(a.substring(0, 16));
  lcd->setCursor(0, 1);
  lcd->print(b.substring(0, 16));
}

void showMsg(char t, String m) {
  String head = (t == 'G') ? "STATUS: OK" : (t == 'W') ? "!! WARNING !!" : "MESSAGE:";
  if (m.length() <= 16) lcdShow(head, m);
  else lcdShow(m.substring(0, 16), m.substring(16));
}

void showLast() {
  if (lastMsg.length()) showMsg(lastType, lastMsg);
  else lcdShow("MARS-SOS", "READY");
}

// ---------- SOS state machine ----------
void startBeep() {
  setOutput(true);
  rgb(1, 0, 0);  // red with every beep
  unsigned long duration = (pulseIndex < 3 || pulseIndex >= 6) ? DOT : DASH;
  phaseEnd = millis() + duration;
  state = BEEPING;
}

// ---------- Bluetooth ----------
void handleLine(String l) {
  l.trim();
  if (l == "SOS") { sosStart = millis(); sosOn = true; }
  else if (l == "STOP") { sosOn = false; }
  else if (l == "TEST") { testReq = true; }
  else if (l.startsWith("GOOD:")) { pendingMsg = l.substring(5); pendingType = 'G'; msgReady = true; }
  else if (l.startsWith("WARN:")) { pendingMsg = l.substring(5); pendingType = 'W'; msgReady = true; }
  else if (l.startsWith("MSG:"))  { pendingMsg = l.substring(4); pendingType = 'M'; msgReady = true; }
}

class RxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) {
    auto v = c->getValue();
    for (size_t i = 0; i < v.length(); i++) {
      char ch = (char)v[i];
      if (ch == '\n' || ch == '\r') {
        if (rxBuf.length()) handleLine(rxBuf);
        rxBuf = "";
      } else if (rxBuf.length() < 80) {
        rxBuf += ch;
      }
    }
  }
};

class SrvCB : public BLEServerCallbacks {
  void onDisconnect(BLEServer *s) { BLEDevice::startAdvertising(); }
};

void setup() {
  pinMode(BUZ, OUTPUT);
  pinMode(LED, OUTPUT);
  pinMode(RGB_R, OUTPUT);
  pinMode(RGB_G, OUTPUT);
  pinMode(RGB_B, OUTPUT);
  setOutput(false);
  rgb(0, 0, 0);

  lcdInit();
  lcdShow("MARS-SOS", "STARTING...");

  BLEDevice::init("MARS-SOS");
  BLEServer *srv = BLEDevice::createServer();
  srv->setCallbacks(new SrvCB());
  BLEService *svc = srv->createService(NUS_SVC);
  BLECharacteristic *rx = svc->createCharacteristic(
    NUS_RX, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCB());
  svc->start();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SVC);
  adv->start();

  rgb(1, 1, 1);          // boot: white + one beep
  blockingBeep(300);
  rgb(0, 0, 0);
  lcdShow("MARS-SOS", "READY");
}

void loop() {
  unsigned long now = millis();

  // New message (shown immediately unless SOS is running)
  if (msgReady) {
    msgReady = false;
    lastMsg = pendingMsg;
    lastType = pendingType;
    setStatusFor(lastType);
    if (!sosOn) {
      showMsg(lastType, lastMsg);
      notify(lastType);
    }
  }

  // Test: LED white, one beep
  if (testReq) {
    testReq = false;
    if (!sosOn) {
      lcdShow("BUZZER TEST", "BEEP");
      rgb(1, 1, 1);
      blockingBeep(300);
      rgb(0, 0, 0);
      showLast();
    }
  }

  // Automatically stop after 2 minutes
  if (sosOn && now - sosStart >= MAX_TIME) sosOn = false;

  // SOS off
  if (!sosOn) {
    setOutput(false);
    state = IDLE;
    pulseIndex = 0;
    if (previousSOS) {
      previousSOS = false;
      rgb(0, 0, 0);
      showLast();
    }
    delay(5);
    return;
  }

  // Start a fresh SOS sequence
  if (!previousSOS) {
    previousSOS = true;
    pulseIndex = 0;
    lcdShow("!!! SOS !!!", "NEED HELP");
    startBeep();
  }

  if ((int32_t)(now - phaseEnd) >= 0) {
    switch (state) {
      case BEEPING:
        setOutput(false);
        rgb(0, 0, 0);
        phaseEnd = now + GAP;
        state = GAP_STATE;
        break;
      case GAP_STATE:
        pulseIndex++;
        if (pulseIndex < 9) {
          startBeep();
        } else {
          phaseEnd = now + PAUSE;
          state = REPEAT_PAUSE;
        }
        break;
      case REPEAT_PAUSE:
        pulseIndex = 0;
        startBeep();
        break;
      default:
        break;
    }
  }
  delay(2);
}

#include <RCSwitch.h>

// --- Pin definitions ---
#define PHOTO_PIN    A0
#define TX_PIN       10
#define EXT_LED_PIN  12

// --- Timing ---
const unsigned long TX_INTERVAL       = 2000;
const unsigned long TX_LED_ON_MS      = 500;
const unsigned long TX_LED_OFF_MS     = 1000;
const unsigned long SERIAL_INTERVAL   = 500;
const unsigned long SLOW_BLINK_PERIOD = 1000;
const unsigned long FAST_BLINK_PERIOD = 200;

// --- Calibration timing ---
const unsigned long CAL_SAMPLE_MS     = 5000;   // Phase A: ambient sampling
const unsigned long CAL_PROMPT_MS     = 15000;  // Phase B: "turn stove on" window
const unsigned long CAL_START_DELAY_MS = 10000; // delay before calibration begins
const unsigned long CAL_FAST_BLINK_MS = 100;

// --- Calibration margins ---
const int MIN_MARGIN       = 80;   // minimum threshold above ambient
const int FALLBACK_MARGIN  = 150;  // if no flame seen, use ambient + this
const float TRIGGER_FRACTION = 0.6; // threshold sits 60% of the way from ambient to peak

// --- Transmitted code ---
const unsigned long CODE = 123456;
const unsigned int  BIT_LENGTH = 24;

// --- RF timing ---
const unsigned int PULSE_LENGTH_US = 185;
const unsigned int REPEAT_TX = 5;

RCSwitch mySwitch = RCSwitch();

// ---- Runtime state ----
int THRESHOLD = 500;   // now dynamic, set during calibration
bool transmitting = false;
unsigned long previousTxMillis = 0;
unsigned long txLedMillis = 0;
unsigned long previousLedMillis = 0;
unsigned long previousSerialMillis = 0;
int lastSensorValue = 0;

// ---- Calibration state machine ----
enum CalState {
  CAL_WAIT,        // startup delay before calibration
  CAL_AMBIENT,     // sample ambient, fast LED blink
  CAL_PROMPT,      // LEDs on, track peak
  CAL_RUNNING      // normal operation
};
CalState calState = CAL_AMBIENT;
unsigned long calStateStart = 0;
unsigned long calLedMillis = 0;
bool calLedOn = false;
long  ambientSum   = 0;
long  ambientCount = 0;
int   ambientMin   = 1023;
int   ambientMax   = 0;
int   ambientAvg   = 0;
int   peakSeen     = 0;   // max reading observed during CAL_PROMPT

// ---- Heartbeat LED pattern ----
static uint8_t ledStep = 0;
static const uint16_t LED_SEQ[] = {
  2000, 50,
   100, 50,
   100, 50,
   100, 10
};
static const uint8_t LED_STEPS = sizeof(LED_SEQ) / sizeof(LED_SEQ[0]);

void setBothLeds(bool on) {
  digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
  digitalWrite(EXT_LED_PIN, on ? HIGH : LOW);
}

void setup() {
  pinMode(PHOTO_PIN, INPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(EXT_LED_PIN, OUTPUT);

  mySwitch.enableTransmit(TX_PIN);
  mySwitch.setProtocol(1);
  mySwitch.setPulseLength(PULSE_LENGTH_US);
  mySwitch.setRepeatTransmit(REPEAT_TX);

  setBothLeds(false);

  Serial.begin(9600);
  Serial.println(F("Photodetector 433MHz TX system started"));
  Serial.println(F("=== CALIBRATION STARTS IN 10 SECONDS ==="));
  Serial.println(F("Keep stove OFF during the 5-second ambient sample."));

  calState = CAL_WAIT;
  calStateStart = millis();
  ambientSum = 0;
  ambientCount = 0;
  ambientMin = 1023;
  ambientMax = 0;
  peakSeen = 0;
}

void loop() {
  unsigned long now = millis();
  int sensorValue = analogRead(PHOTO_PIN);
  lastSensorValue = sensorValue;

  // ==========================================================
  //  CALIBRATION STATE MACHINE
  // ==========================================================
  switch (calState) {

    case CAL_WAIT: {
      // Keep both LEDs on during the startup delay before calibration.
      setBothLeds(true);
      if (now - calStateStart >= CAL_START_DELAY_MS) {
        Serial.println(F("=== CALIBRATION: keep stove OFF for 5 seconds ==="));
        calState = CAL_AMBIENT;
        calStateStart = now;
        ambientSum = 0;
        ambientCount = 0;
        ambientMin = 1023;
        ambientMax = 0;
        calLedMillis = now;
        calLedOn = false;
        setBothLeds(false);
      }
      return;
    }

    case CAL_AMBIENT: {
      // Fast blink while waiting for the stove-off ambient sample.
      if (now - calLedMillis >= CAL_FAST_BLINK_MS) {
        calLedMillis = now;
        calLedOn = !calLedOn;
        setBothLeds(calLedOn);
      }
      ambientSum += sensorValue;
      ambientCount++;
      if (sensorValue < ambientMin) ambientMin = sensorValue;
      if (sensorValue > ambientMax) ambientMax = sensorValue;

      if (now - calStateStart >= CAL_SAMPLE_MS) {
        ambientAvg = (ambientCount > 0) ? (int)(ambientSum / ambientCount) : 0;
        Serial.print(F("Ambient avg=")); Serial.print(ambientAvg);
        Serial.print(F(" min="));        Serial.print(ambientMin);
        Serial.print(F(" max="));        Serial.println(ambientMax);
        Serial.println(F("=== TURN STOVE ON NOW (15 s window) ==="));
        peakSeen = ambientMax;
        calState = CAL_PROMPT;
        calStateStart = now;
      }
      return; // don't do TX / heartbeat while calibrating
    }

    case CAL_PROMPT: {
      // LEDs stay on throughout calibration; track peak sensor value.
      setBothLeds(true);
      if (sensorValue > peakSeen) peakSeen = sensorValue;

      if (now - calStateStart >= CAL_PROMPT_MS) {
        // Compute threshold
        int rise = peakSeen - ambientAvg;
        int computed;
        if (rise >= MIN_MARGIN) {
          computed = ambientAvg + (int)(rise * TRIGGER_FRACTION);
          Serial.print(F("Flame detected during cal. peak="));
          Serial.print(peakSeen);
          Serial.print(F(" rise=")); Serial.print(rise);
        } else {
          computed = ambientAvg + FALLBACK_MARGIN;
          Serial.print(F("No flame seen during cal. Using fallback. peak="));
          Serial.print(peakSeen);
        }

        // Clamp
        if (computed < ambientAvg + MIN_MARGIN) computed = ambientAvg + MIN_MARGIN;
        if (computed > 1023) computed = 1023;
        THRESHOLD = computed;

        Serial.print(F(" => THRESHOLD = "));
        Serial.println(THRESHOLD);
        Serial.println(F("=== CALIBRATION COMPLETE ==="));

        calState = CAL_RUNNING;
        previousLedMillis = now;
        previousTxMillis = now;
        previousSerialMillis = now;
        ledStep = 0;
        transmitting = false;
        setBothLeds(false);
        Serial.println(F("Sensor | State"));
        Serial.println(F("-------+-------"));
      }
      return;
    }

    case CAL_RUNNING:
      break; // fall through to normal operation
  }

  // ==========================================================
  //  NORMAL OPERATION
  // ==========================================================
  bool aboveThreshold = (sensorValue > THRESHOLD);

  // 1. Transmission logic
  if (aboveThreshold) {
    if (!transmitting) {
      transmitting = true;
      previousTxMillis = now;
      mySwitch.send(CODE, BIT_LENGTH);
      txLedMillis = now;
      setBothLeds(true);
      Serial.print(F(">>> TRANSMIT STARTED (sensor: "));
      Serial.print(sensorValue);
      Serial.print(F(", thr: ")); Serial.print(THRESHOLD);
      Serial.println(F(") <<<"));
    } else {
      if (now - previousTxMillis >= TX_INTERVAL) {
        previousTxMillis = now;
        mySwitch.send(CODE, BIT_LENGTH);
        txLedMillis = now;
        setBothLeds(true);
      }
    }
  } else {
    if (transmitting) {
      transmitting = false;
      setBothLeds(false);
      txLedMillis = now;
      previousLedMillis = now;
      ledStep = 0;
      Serial.print(F("--- TRANSMIT STOPPED (sensor: "));
      Serial.print(sensorValue);
      Serial.println(F(") ---"));
    }
  }

  // 2. LED status
  if (transmitting) {
    // Transmission pattern: 500 ms on, then 1 second off.
    bool ledOn = (digitalRead(LED_BUILTIN) == HIGH);
    unsigned long ledDuration = ledOn ? TX_LED_ON_MS : TX_LED_OFF_MS;
    if (now - txLedMillis >= ledDuration) {
      txLedMillis = now;
      setBothLeds(!ledOn);
    }
  } else {
    // Normal idle heartbeat pattern.
    if (now - previousLedMillis >= LED_SEQ[ledStep]) {
      previousLedMillis = now;
      ledStep = (ledStep + 1) % LED_STEPS;
      bool on = (ledStep % 2 == 1);
      setBothLeds(on);
    }
  }

  // 3. Serial output
  if (now - previousSerialMillis >= SERIAL_INTERVAL) {
    previousSerialMillis = now;
    Serial.print(sensorValue);
    Serial.print(F("   | "));
    if (transmitting) {
      Serial.print(F("TX ACTIVE"));
      if (now - previousTxMillis < 100) Serial.print(F(" [SENT]"));
    } else {
      Serial.print(F("IDLE     "));
    }
    Serial.print(F("  thr="));
    Serial.print(THRESHOLD);
    Serial.println();

    static int lineCount = 0;
    lineCount++;
    if (lineCount >= 20) {
      lineCount = 0;
      Serial.println(F("-------+-------"));
      Serial.println(F("Sensor | State"));
      Serial.println(F("-------+-------"));
    }
  }
}
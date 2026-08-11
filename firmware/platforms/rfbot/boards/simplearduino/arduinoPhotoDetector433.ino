#include <RCSwitch.h>

// --- Pin definitions ---
#define PHOTO_PIN    A0         // Photodetector analog input
#define TX_PIN       10         // 433 MHz transmitter data pin
#define EXT_LED_PIN  12         // External LED

// --- Threshold & timing ---
const int THRESHOLD = 500;
const unsigned long TX_INTERVAL = 1000;   // 1 Hz when above threshold

// --- LED blink intervals ---
const unsigned long SLOW_BLINK_PERIOD = 1000;  // 500 ms on / 500 ms off
const unsigned long FAST_BLINK_PERIOD = 200;   // 100 ms on / 100 ms off

// --- Serial output interval ---
const unsigned long SERIAL_INTERVAL = 500;

// --- Transmitted code ---
const unsigned long CODE = 123456;
const unsigned int  BIT_LENGTH = 24;

// --- RF timing: MUST match ESP-IDF receiver config ---
const unsigned int PULSE_LENGTH_US = 185;   // 1T = 185 µs (matches IDF Protocol 1)
const unsigned int REPEAT_TX = 5;           // 5 repeats per send() for robust RX

RCSwitch mySwitch = RCSwitch();

bool transmitting = false;
unsigned long previousTxMillis = 0;
bool ledState = LOW;
unsigned long previousLedMillis = 0;
unsigned long previousSerialMillis = 0;
int lastSensorValue = 0;

void toggleLEDs() {
  ledState = !ledState;
  digitalWrite(LED_BUILTIN, ledState);
  digitalWrite(EXT_LED_PIN, ledState);
}

void setup() {
  pinMode(PHOTO_PIN, INPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(EXT_LED_PIN, OUTPUT);

  // ============================================================
  //  RF CONFIG: these 3 lines make the signal match the ESP-IDF RX
  // ============================================================
  mySwitch.enableTransmit(TX_PIN);
  mySwitch.setProtocol(1);                    // Protocol 1 (EV1527 / PT2262 style)
  mySwitch.setPulseLength(PULSE_LENGTH_US);   // 185 µs per time-unit
  mySwitch.setRepeatTransmit(REPEAT_TX);      // send 5 back-to-back bursts

  digitalWrite(LED_BUILTIN, LOW);
  digitalWrite(EXT_LED_PIN, LOW);

  Serial.begin(9600);
  Serial.println("Photodetector 433MHz TX system started");
  Serial.print("Threshold: ");      Serial.println(THRESHOLD);
  Serial.print("Pulse length: ");   Serial.print(PULSE_LENGTH_US); Serial.println(" us");
  Serial.print("Protocol: 1, Bits: "); Serial.println(BIT_LENGTH);
  Serial.println("Sensor | State");
  Serial.println("-------+-------");
}

void loop() {
  int sensorValue = analogRead(PHOTO_PIN);
  lastSensorValue = sensorValue;
  bool aboveThreshold = (sensorValue > THRESHOLD);
  unsigned long now = millis();

  // 1. Transmission logic
  if (aboveThreshold) {
    if (!transmitting) {
      transmitting = true;
      previousTxMillis = now;
      mySwitch.send(CODE, BIT_LENGTH);
      Serial.print(">>> TRANSMIT STARTED (sensor: ");
      Serial.print(sensorValue);
      Serial.println(") <<<");
    } else {
      if (now - previousTxMillis >= TX_INTERVAL) {
        previousTxMillis = now;
        mySwitch.send(CODE, BIT_LENGTH);
      }
    }
  } else {
    if (transmitting) {
      transmitting = false;
      Serial.print("--- TRANSMIT STOPPED (sensor: ");
      Serial.print(sensorValue);
      Serial.println(") ---");
    }
  }

  // 2. LED blinking
  unsigned long blinkPeriod = transmitting ? FAST_BLINK_PERIOD : SLOW_BLINK_PERIOD;
  if (now - previousLedMillis >= blinkPeriod / 2) {
    previousLedMillis = now;
    toggleLEDs();
  }

  // 3. Serial output
  if (now - previousSerialMillis >= SERIAL_INTERVAL) {
    previousSerialMillis = now;
    Serial.print(sensorValue);
    Serial.print("   | ");
    if (transmitting) {
      Serial.print("TX ACTIVE");
      if (now - previousTxMillis < 100) {
        Serial.print(" [SENT]");
      }
    } else {
      Serial.print("IDLE     ");
    }
    Serial.println();

    static int lineCount = 0;
    lineCount++;
    if (lineCount >= 20) {
      lineCount = 0;
      Serial.println("-------+-------");
      Serial.println("Sensor | State");
      Serial.println("-------+-------");
    }
  }
}
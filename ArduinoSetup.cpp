#include <SoftwareSerial.h>

SoftwareSerial btSerial(10, 11); // Arduino to HC-05 RX = pin 10, TX = pin 11

const uint8_t MQ2_ANALOG_PIN  = A0;
const uint8_t MQ2_DIGITAL_PIN = 2;

const unsigned long WARMUP_MS     = 20000;
const unsigned long SEND_INTERVAL = 1000; // one reading per second

unsigned long lastSend = 0;

void setup() {
  Serial.begin(9600); // USB debug
  delay(3000);
  btSerial.begin(38400); // HC-05 default data-mode baud

  pinMode(MQ2_DIGITAL_PIN, INPUT);

  Serial.println("MQ-2 warming up...");
  delay(WARMUP_MS);
  Serial.println(F("MQ-2 ready. Sending over HC-05."));
}

void loop() {
  unsigned long now = millis();
  if (now - lastSend < SEND_INTERVAL) return;
  lastSend = now;

  int raw = analogRead(MQ2_ANALOG_PIN);

  // One line: "MQ2,<raw>\n"
  btSerial.println("MQ2," + String(raw));

  // Serial monitor output
  Serial.println("MQ2," + String(raw));
}
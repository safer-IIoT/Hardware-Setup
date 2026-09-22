const uint8_t MQ2_ANALOG_PIN  = A0;
const uint8_t MQ2_DIGITAL_PIN = 2;

const unsigned long WARMUP_MS     = 20000;

void setup() {
  Serial.begin(9600);

  pinMode(MQ2_DIGITAL_PIN, INPUT);

  Serial.println(F("MQ-2 warming up..."));
  delay(WARMUP_MS);
  Serial.println(F("MQ-2 ready."));
}

void loop() {
  int raw = analogRead(MQ2_ANALOG_PIN);
  
  Serial.print(F("raw=")); 
  Serial.print(raw);

  delay(500);
}
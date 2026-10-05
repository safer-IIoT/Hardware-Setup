#include <SoftwareSerial.h>

SoftwareSerial btSerial(10, 11);

const uint8_t MQ2_ANALOG_PIN  = A0;
const uint8_t MQ2_DIGITAL_PIN = 2;

const unsigned long WARMUP_MS        = 20000;
const unsigned long SAMPLE_INTERVAL = 1000;
const unsigned long BATCH_INTERVAL  = 15000;

unsigned long lastSampleTime = 0;
unsigned long lastBatchTime  = 0;

const int MAX_SAMPLES = 20;
int valuesBuffer[MAX_SAMPLES];
int currentSampleIndex = 0;

void setup() 
{
  Serial.begin(9600); 
  btSerial.begin(38400); 

  pinMode(MQ2_DIGITAL_PIN, INPUT);

  Serial.println(F("MQ-2 warming up..."));
  delay(WARMUP_MS);
  Serial.println(F("MQ-2 ready"));
  
  unsigned long bootTime = millis();
  lastSampleTime = bootTime;
  lastBatchTime = bootTime;
}

void loop() 
{
  unsigned long now = millis();

  if (now - lastSampleTime >= SAMPLE_INTERVAL) 
  {
    lastSampleTime += SAMPLE_INTERVAL;

    int rawValue = analogRead(MQ2_ANALOG_PIN);
    
    if (currentSampleIndex < MAX_SAMPLES) 
    {
      valuesBuffer[currentSampleIndex] = rawValue;
      currentSampleIndex++;
    }
  }

  if (now - lastBatchTime >= BATCH_INTERVAL) 
  {
    lastBatchTime += BATCH_INTERVAL;

    if (currentSampleIndex > 0) 
    {
      String payload = "[";
      for (int i = 0; i < currentSampleIndex; i++) 
      {
        payload += String(valuesBuffer[i]);
        if (i < currentSampleIndex - 1) 
        {
          payload += ",";
        }
      }
      payload += "]";

      btSerial.println(payload);
      Serial.println(payload);

      currentSampleIndex = 0;
    }
  }
}
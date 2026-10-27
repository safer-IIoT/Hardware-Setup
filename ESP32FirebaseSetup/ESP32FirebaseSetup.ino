#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "time.h"
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

#include "config.h"
#include "secretconfig.h"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

unsigned long sendDataPrevMillis = 0;
const unsigned long sendInterval = FIREBASE_INTERVAL;

const char* ntpServer = NTP_SERVER;
const long  gmtOffset_sec = 19800;
const int   daylightOffset_sec = 0; 

String getFormattedTimestamp();
void verifyNetworkConnectivity();

void setup() {
  Serial.begin(115200);

  pinMode(4, INPUT);

  // Connect to Wi-Fi network
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.println("Not connected to Wi-Fi");
    delay(300);
  }
  Serial.println("Connected to Wi-Fi");

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("Synchronizing time via NTP");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo, 5000)) { 
    Serial.println("Time was not synchronized");
    delay(500);
  }
  Serial.println("Time was synchronized");

  Serial1.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = EMAIL;
  auth.user.password = PASSWORD;

  config.token_status_callback = tokenStatusCallback; 

  // Set safe SSL buffer margins to minimize memory overhead crashes
  fbdo.setBSSLBufferSize(2048 /* Rx */, 2048 /* Tx */);
  
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  Firebase.RTDB.setMaxRetry(&fbdo, 5);          
}

void loop() {
  verifyNetworkConnectivity();

  unsigned long currentMillis = millis();

  int rawAnalogValue = analogRead(4); 
  int calibratedGasScore = map(rawAnalogValue, 0, 3102, 0, 100);
  calibratedGasScore = constrain(calibratedGasScore, 0, 100);

  if(currentMillis - sendDataPrevMillis > IOTHUB_INTERVAL)
  {
    sendDataPrevMillis = currentMillis;
    Serial1.print(calibratedGasScore);
    Serial.println(calibratedGasScore);
  }

  if (Firebase.ready() && (currentMillis - sendDataPrevMillis > sendInterval || sendDataPrevMillis == 0)) {
    sendDataPrevMillis = currentMillis;

    FirebaseJsonArray arr;
    arr.add(calibratedGasScore); 

    FirebaseJson logEntry;
    logEntry.set("values", arr);

    String timeKey = getFormattedTimestamp();
    if (timeKey == "") {
      timeKey = "fallback_" + String(currentMillis);
    }

    String currentPath = String("/data/") + DEVICE_ID + String("/") + timeKey;
    Serial.printf("Writing log entry to path: %s\n", currentPath.c_str());

    bool success = false;
    int retryCount = 0;
    const int maxRetries = 3; 
    
    while (!success && retryCount < maxRetries) {
      verifyNetworkConnectivity();
      
      if (Firebase.ready()) {
        success = Firebase.RTDB.setJSON(&fbdo, currentPath.c_str(), &logEntry);
      }
      
      if (success) {
        Serial.println("Data successfully saved");
      } else {
        retryCount++;
        Serial.printf("Data saving attempt %d failed. Reason: %s\n", retryCount, fbdo.errorReason().c_str());
        if (retryCount < maxRetries) {
          delay(2000 * retryCount); 
        }
      }
    }
  }

  delay(10);
}


void verifyNetworkConnectivity() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Re-establishing network handshake");
    WiFi.disconnect();
    WiFi.reconnect();
    
    unsigned long startTimeout = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTimeout < 8000) {
      delay(200);
      Serial.println("Not connected to Wi-Fi");
    }
    Serial.println("Wi-Fi restabilized.");
  }
}

String getFormattedTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain local time from internal clock");
    return "";
  }
  
  char buffer[32];
  strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", &timeinfo);
  return String(buffer);
}
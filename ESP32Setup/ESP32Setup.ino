#include "BluetoothSerial.h"
#include <WiFi.h>
#include "esp_log.h"
#include "config.h"
#include <RTClib.h>

#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

BluetoothSerial ESP_BT;
uint8_t hc05_address[] = HC05_ADDRESS;

volatile bool isWifiLinked = false; 
volatile bool wifiDisconnected = false;
String latest15SecArray = "";
portMUX_TYPE dataMutex = portMUX_INITIALIZER_UNLOCKED;

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig fbConfig;

RTC_DS3231 rtc;

void connectToWiFi(void * pvParameters);
void connectToHC05(void * pvParameters);
void WiFiEvent(WiFiEvent_t event);
void sendToFirebase(String jsonPayload, String targetDate);

void setup() 
{
  esp_log_level_set("*", ESP_LOG_NONE); 
  Serial.begin(115200);
  delay(1000);
  
  //Connect to Wi-Fi
  WiFi.onEvent(WiFiEvent);
  xTaskCreatePinnedToCore(connectToWiFi, "WiFiTask", 12288, NULL, 1, NULL, 0);

  //Connect to Arduino <-> HC-05
  xTaskCreatePinnedToCore(connectToHC05, "BluetoothTask", 4096, NULL, 1, NULL, 1);
}

void loop() 
{
  vTaskDelay(pdMS_TO_TICKS(1000)); 
}

void WiFiEvent(WiFiEvent_t event) {
  switch(event) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      wifiDisconnected = true;
      isWifiLinked = false;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      wifiDisconnected = false;
      break;
    default:
      break;
  }
}

//Checks whether Wi-Fi is connected
void connectToWiFi(void * pvParameters)
{
  Serial.println("Wi-Fi setup initialized.");
  
  bool wasConnected = false;

  WiFi.persistent(false);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_11dBm); 
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 50) 
  {
    vTaskDelay(pdMS_TO_TICKS(500));
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) 
  {
    WiFi.setSleep(false);
    fbConfig.host = FIREBASE_HOST;
    fbConfig.signer.tokens.legacy_token = FIREBASE_AUTH;
    Firebase.begin(&fbConfig, &auth);
    Firebase.reconnectWiFi(true);    
    isWifiLinked = true;
    Serial.println("Wi-Fi connected successfully"); 
  } 
  else 
  {
    wifiDisconnected = true;
  }

  DynamicJsonDocument firebaseDoc(8192);
  JsonArray firebaseMasterArray = firebaseDoc.to<JsonArray>();
  
  uint32_t lastFirebaseSend = millis();
  uint32_t lastConnectionCheck = millis();

  DateTime now = rtc.now();
  char dateBuffer[12]; 
  sprintf(dateBuffer, "%04d/%02d/%02d", now.year(), now.month(), now.day());
  String currentBatchDate = String(dateBuffer);

  while(1) 
  {
    vTaskDelay(pdMS_TO_TICKS(10000));
    uint32_t currentMillis = millis();

    if (isWifiLinked) {
      if (currentMillis - lastFirebaseSend >= 60000) 
      {
        lastFirebaseSend = currentMillis;

        if (firebaseMasterArray.size() > 0) 
        {
          JsonDocument finalFirebaseDoc;
          finalFirebaseDoc["node_id"] = NODE_ID;
          finalFirebaseDoc["readings"] = firebaseMasterArray;

          String firebaseOutput;
          serializeJson(finalFirebaseDoc, firebaseOutput);
          sendToFirebase(firebaseOutput, currentBatchDate);

          firebaseDoc.clear();
          firebaseMasterArray = firebaseDoc.to<JsonArray>();
          }
      }
    }
  }

  bool isDown = (wifiDisconnected || WiFi.status() != WL_CONNECTED);

  if (!isDown) 
  {
    int currentRSSI = WiFi.RSSI();
    if (currentRSSI >= 0) 
    { 
      isDown = true; 
    }
  }

  if (isDown) 
  {
    if (wasConnected) 
    {
      Serial.println("Wi-Fi link lost. (Bluetooth will keep running independently)...");
      wasConnected = false; 
    }
    
    WiFi.disconnect(true);
    vTaskDelay(pdMS_TO_TICKS(1000)); 
    
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_11dBm);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    int retryAttempts = 0;
    while (WiFi.status() != WL_CONNECTED && retryAttempts < 30) 
    { 
      vTaskDelay(pdMS_TO_TICKS(500));
      retryAttempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED && WiFi.RSSI() < 0) 
    {
      WiFi.setSleep(false);
      wifiDisconnected = false; 
      wasConnected = true;
      Serial.println("Wi-Fi reconnected successfully.");
    }    
  } 
  else 
  {
    wasConnected = true;
    wifiDisconnected = false;
    isWifiLinked = true;
    Serial.print("[RSSI: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm] Wi-Fi Link stable.");
  }
}

//Checks whether Arduino <-> HC-05 is connected
void connectToHC05(void * pvParameters) 
{
  Serial.println("Bluetooth setup initialized. Standing by for initial Wi-Fi link...");

  while (!isWifiLinked) 
  {
    vTaskDelay(pdMS_TO_TICKS(100)); 
  }
  
  Serial.println("Wi-Fi link verified. Starting Bluetooth setup.");

  // Initialize Bluetooth hardware once
  if (!ESP_BT.begin("ESP32_Master", true)) 
  {
    Serial.println("Error: Bluetooth failed to initialize.");
    vTaskDelete(NULL); 
  }
  Serial.println("Connecting to HC-05...");

  while(1) 
  {
    if (!ESP_BT.connected()) 
    {
      Serial.println("Attempting to reconnect to HC-05...");
      
      if (ESP_BT.connect(hc05_address)) 
      {
        Serial.println("Connected securely to Arduino <-> HC-05");
      } 
      else 
      {
        Serial.println("Not connected to Arduino <-> HC-05. Retrying...");
        vTaskDelay(pdMS_TO_TICKS(2000)); 
        continue; 
      }
    }

    if (ESP_BT.available()) 
    {
      String incomingData = ESP_BT.readStringUntil('\n');
      incomingData.trim(); 
      Serial.println(incomingData);

      if (incomingData.length() > 0) 
      {
        latest15SecArray = incomingData;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void sendToFirebase(String jsonPayload, String targetDate) 
{
  if (Firebase.ready()) 
  {
    String path = "/nodes/" + String(NODE_ID) + "/" + targetDate;
    
    FirebaseJson firebaseJsonObj;
    firebaseJsonObj.setJsonData(jsonPayload);
    
    if (Firebase.RTDB.pushJSON(&fbdo, path, &firebaseJsonObj)) 
    {
      Serial.println("→ [Firebase] 1-Min Nested Aggregation Complete.");
    } 
    else 
    {
      Serial.printf("Firebase error: %s\n", fbdo.errorReason().c_str());
    }
  }
}
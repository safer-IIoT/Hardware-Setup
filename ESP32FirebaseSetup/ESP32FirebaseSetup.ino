#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "time.h"
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

#include "config.h"
#include "secretconfig.h"

constexpr int MQ_2_PIN = 4;
constexpr uint32_t BAUDRATE = 115200;
constexpr size_t IOTHUB_NUM_VALUES = IOTHUB_INTERVAL/ 1000;
constexpr size_t FIREBASE_NUM_VALUES = FIREBASE_INTERVAL/ 1000;
constexpr int DECIMALS = 4;

static_assert(RX_PIN != MQ_2_PIN && TX_PIN != MQ_2_PIN, "UART pins must not use the MQ-2 sensor pin");

const char* ntpServer = NTP_SERVER;
const long  gmtOffsetSec = 19800;   // UTC+5:30
const int   daylightOffsetSec = 0;

FirebaseData   fbdo;
FirebaseAuth   auth;
FirebaseConfig config;

double values[IOTHUB_NUM_VALUES] = {0};
double firebaseValues[FIREBASE_NUM_VALUES] = {0};
unsigned long lastReadMillis = 0;
size_t currentESP32Reading = 0;
size_t currentFirebaseReading = 0;

String getFormattedTimestamp();
void verifyNetworkConnectivity();

// ---------- sensor ----------
void readSensorValue(double* esp32Values, double* firebaseValues) {
  int raw = analogRead(MQ_2_PIN);
  double score = constrain((raw * 100.0 / 3102.0), 0.0, 100.0);
  esp32Values[currentESP32Reading++] = score;
  firebaseValues[currentFirebaseReading++] = score;
}

uint8_t xorChecksum(const char* s, size_t len) {
  uint8_t cs = 0;
  for (size_t i = 0; i < len; i++) cs ^= (uint8_t)s[i];
  return cs;
}

bool sendDoubles(HardwareSerial& port, const double* v, size_t n) {
  char buf[512];
  size_t len = 0;
  buf[len++] = '$';

  for (size_t i = 0; i < n; i++) {
    int w = snprintf(buf + len, sizeof(buf) - len, "%s%.*f", i ? "," : "", DECIMALS, v[i]);
    if (w < 0 || (size_t)w >= sizeof(buf) - len) return false;
    len += w;
  }

  uint8_t cs = xorChecksum(buf + 1, len - 1);
  int w = snprintf(buf + len, sizeof(buf) - len, "*%02X\n", cs);
  if (w < 0 || (size_t)w >= sizeof(buf) - len) return false;
  len += w;

  port.write((const uint8_t*)buf, len);
  Serial.write((const uint8_t*)buf, len);
  return true;
}

void logToFirebase(const double* v, size_t n) {
  FirebaseJsonArray arr;
  for (size_t i = 0; i < n; i++) arr.add(v[i]);

  FirebaseJson logEntry;
  logEntry.set("values", arr);

  String timeKey = getFormattedTimestamp();
  if (timeKey == "") timeKey = "fallback_" + String(millis());

  String currentPath = String("/data/") + DEVICE_ID + "/" + timeKey;
  Serial.printf("Writing log entry to path: %s\n", currentPath.c_str());

  bool success = false;
  const int maxRetries = 3;
  for (int retry = 1; !success && retry <= maxRetries; retry++) {
    verifyNetworkConnectivity();
    if (Firebase.ready())
      success = Firebase.RTDB.setJSON(&fbdo, currentPath.c_str(), &logEntry);

    if (success) {
      Serial.println("Data successfully saved");
    } else {
      Serial.printf("Data saving attempt %d failed. Reason: %s\n",
                    retry, fbdo.errorReason().c_str());
      if (retry < maxRetries) delay(2000 * retry);
    }
  }
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("Wi-Fi: connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi not connected");
  }
  Serial.printf("Wi-Fi: connected, IP %s\n", WiFi.localIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);

  Serial1.begin(BAUDRATE, SERIAL_8N1, RX_PIN, TX_PIN);

  /*connectWiFi();

  configTime(gmtOffsetSec, daylightOffsetSec, ntpServer);
  Serial.println("Synchronizing time via NTP");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo, 5000)) {
    Serial.println("Time was not synchronized");
    delay(500);
  }
  Serial.println("Time was synchronized");

  config.api_key      = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email     = EMAIL;
  auth.user.password  = PASSWORD;
  config.token_status_callback = tokenStatusCallback;

  fbdo.setBSSLBufferSize(2048, 2048);

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  Firebase.RTDB.setMaxRetry(&fbdo, 5);*/
}

void loop() {
  //verifyNetworkConnectivity();
  unsigned long now = millis();

  if (now - lastReadMillis >= 1000) 
  {
    lastReadMillis = now;
    readSensorValue(values, firebaseValues);

    if (currentESP32Reading == IOTHUB_NUM_VALUES) 
    {
      sendDoubles(Serial1, values, IOTHUB_NUM_VALUES);
      Serial.println("Sent sensor readings");
      currentESP32Reading = 0;
    }
  }

  /*if (currentFirebaseReading == FIREBASE_NUM_VALUES) {
    logToFirebase(firebaseValues, FIREBASE_NUM_VALUES);
    currentFirebaseReading = 0;
  }*/

  delay(10);
}

void verifyNetworkConnectivity() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Re-establishing network handshake");
    WiFi.disconnect();
    WiFi.reconnect();

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
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
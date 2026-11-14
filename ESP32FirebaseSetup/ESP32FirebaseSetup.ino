#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "time.h"
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include "FS.h"
#include "LittleFS.h"

#include "config.h"
#include "secretconfig.h"
#include "mq2_lr_model.h"

constexpr int MQ_2_PIN = 4;
constexpr uint32_t BAUDRATE = 115200;
constexpr size_t IOTHUB_NUM_VALUES = IOTHUB_INTERVAL/ 1000;
constexpr size_t FIREBASE_NUM_VALUES = FIREBASE_INTERVAL/ 1000;
constexpr int DECIMALS = 4;
constexpr int LED_PIN = 2;
constexpr float DIVIDER_RATIO = 2.0f;

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
float mlWindow[mq2lr::WINDOW];

String getFormattedTimestamp();
void handleWiFi();

void writeLog(const char* level, const String& message) 
{
  Serial.printf("[%s] %s\n", level, message.c_str());

  if (!fsMounted) 
    return;

  // leave at least 32 KB free for LittleFS overhead
  if (LittleFS.totalBytes() - LittleFS.usedBytes() < 32768) 
  {
    Serial.println("[WARN] LittleFS low on space! Skipping flash log write.");
    return;
  }

  char filePath[32];
  char timePrefix[24];

  if (timeSynced) 
  {
    time_t now = time(nullptr) + TIME_OFFSET_S;
    struct tm t;
    gmtime_r(&now, &t);
    // Format filename: /info-yyyyMMdd.txt or /error-yyyyMMdd.txt
    snprintf(filePath, sizeof(filePath), "/%s-%04d%02d%02d.txt", level, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    // Format line timestamp: [HH:MM:SS]
    snprintf(timePrefix, sizeof(timePrefix), "[%02d:%02d:%02d]", t.tm_hour, t.tm_min, t.tm_sec);
  } 
  else 
  {
    snprintf(filePath, sizeof(filePath), "/%s-unsynced.txt", level);
    snprintf(timePrefix, sizeof(timePrefix), "[+%lums]", millis());
  }

  File file;

  if (!LittleFS.exists(filePath)) 
  {
    file = LittleFS.open(filePath, FILE_WRITE);
  } 
  else 
  {
    file = LittleFS.open(filePath, FILE_APPEND);
  }

  if (file) 
  {
    file.printf("%s %s\n", timePrefix, message.c_str());
    file.close();
  } 
  else 
  {
    Serial.printf("[FS] Failed to open %s for appending\n", filePath);
  }
}

void logInfo(const String& msg)
{ 
  writeLog("info", msg); 
}

void logError(const String& msg) 
{ 
  writeLog("error", msg); 
}

void readSensorValue(double* esp32Values, double* firebaseValues) 
{
  int raw = analogRead(MQ_2_PIN);
  double score = constrain((raw * 100.0 / 3102.0), 0.0, 100.0);

  mlWindow[currentESP32Reading] = mq2lr::countsFromMilliVolts(analogReadMilliVolts(MQ_2_PIN), DIVIDER_RATIO);

  esp32Values[currentESP32Reading++] = score;
  firebaseValues[currentFirebaseReading++] = score;
}

uint8_t xorChecksum(const char* s, size_t len) 
{
  uint8_t cs = 0;

  for (size_t i = 0; i < len; i++)
    cs ^= (uint8_t)s[i];

  return cs;
}

bool sendDoubles(HardwareSerial& port, const double* v, size_t n) 
{
  char buf[512];
  size_t len = 0;
  buf[len++] = '$';

  for (size_t i = 0; i < n; i++) 
  {
    int w = snprintf(buf + len, sizeof(buf) - len, "%s%.*f", i ? "," : "", DECIMALS, v[i]);
    
    if (w < 0 || (size_t)w >= sizeof(buf) - len) 
      return false;

    len += w;
  }

  uint8_t cs = xorChecksum(buf + 1, len - 1);
  int w = snprintf(buf + len, sizeof(buf) - len, "*%02X\n", cs);

  if (w < 0 || (size_t)w >= sizeof(buf) - len) 
    return false;
  
  len += w;

  port.write((const uint8_t*)buf, len);
  Serial.write((const uint8_t*)buf, len);
  
  return true;
}

void logToFirebase(const double* v, size_t n) 
{
  FirebaseJsonArray arr;
  
  for (size_t i = 0; i < n; i++) 
    arr.add(v[i]);

  FirebaseJson logEntry;
  logEntry.set("values", arr);

  String timeKey = getFormattedTimestamp();
  
  if (timeKey == "") 
    timeKey = "fallback_" + String(millis());

  String currentPath = String("/data/") + DEVICE_ID + "/" + timeKey;
  Serial.printf("Writing log entry to path: %s\n", currentPath.c_str());

  bool success = false;
  const int maxRetries = 3;

  for (int retry = 1; !success && retry <= maxRetries; retry++) 
  {
    handleWiFi();

    if (Firebase.ready())
    {
      success = Firebase.RTDB.setJSON(&fbdo, currentPath.c_str(), &logEntry);
    }

    if (success) 
    {
      logInfo("Data successfully saved in Firebase");
    } 
    else 
    {
      logError(String("Data saving attempt ") +  retry +  String(" failed. Reason: ") + String(fbdo.errorReason().c_str()));
      
      if (retry < maxRetries)
      {
        delay(2000 * retry);
      }
    }
  }
}

void connectWiFi() 
{
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFISSID, WIFIPASSWORD);
}

void handleNTP() 
{
  if (WiFi.status() != WL_CONNECTED) 
  {
    timeSynced = false;
    return;
  }

  if (timeSynced)
    return;

  static uint32_t lastNtpCheck = 0;
  time_t now = time(nullptr);

  if (now < 1700000000) 
  {
    if (millis() - lastNtpCheck > 5000) 
    { 
      lastNtpCheck = millis();
      logError("NTP: Time not set yet");
      configTime(0, 0, "pool.ntp.org", "://google.com"); 
    }
  } 
  else 
  {
    timeSynced = true;
    
    char timestamp[32];
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S UTC", &timeinfo);
  }
}

void setup() 
{
  logInfo("Beginning ESP32S3 process");

  logInfo("Serial initialization started");

  Serial.begin(115200);
  Serial1.begin(BAUDRATE, SERIAL_8N1, RX_PIN, TX_PIN);

  logInfo("Serial initialization ended");

  pinMode(LED_PIN, OUTPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(MQ_2_PIN, ADC_11db);

  logInfo("Wi-Fi initialization started");

  connectWiFi();

  logInfo("Wi-Fi initialization ended");

  logInfo("Firebase initialization started");

  config.api_key      = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email     = EMAIL;
  auth.user.password  = PASSWORD;
  config.token_status_callback = tokenStatusCallback;

  fbdo.setBSSLBufferSize(2048, 2048);

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  Firebase.RTDB.setMaxRetry(&fbdo, 5);

  logInfo("Firebase initialization ended");
}

void loop() 
{
  handleWiFi();

  handleNTP();

  unsigned long now = millis();

  if (now - lastReadMillis >= 1000) 
  {
    lastReadMillis = now;
    readSensorValue(values, firebaseValues);

    if (currentESP32Reading == IOTHUB_NUM_VALUES) 
    {
      sendDoubles(Serial1, values, IOTHUB_NUM_VALUES);
      logInfo("Sent sensor readings");

      logInfo("Using ML model to detect gases/vapors/mixtures");

      float p[mq2lr::N_CLASSES];
      mq2lr::GasClass c = mq2lr::classify(mlWindow, p);
      bool smoke = (c == mq2lr::SMOKE || c == mq2lr::MIXTURE);
      digitalWrite(LED_PIN, smoke ? HIGH : LOW);
      Serial.printf("edge: %s (%.0f%%)\n", mq2lr::CLASS_NAMES[c], 100 * p[c]);

      currentESP32Reading = 0;
    }
  }

  if (currentFirebaseReading == FIREBASE_NUM_VALUES) 
  {
    logToFirebase(firebaseValues, FIREBASE_NUM_VALUES);
    currentFirebaseReading = 0;
  }

  delay(10);
}

void handleWiFi() 
{
  uint32_t currentTime = millis();

  if (WiFi.status() == WL_CONNECTED)
  {
    if (currentTime - lastPrintTime > 10000)
    {
      lastPrintTime = currentTime;
    }

    return;
  }

  if (currentTime - lastPrintTime > 3000) 
  {
    logError("Wi-Fi is not available");

    lastPrintTime = currentTime;
    int currentStatus = WiFi.status();
    Serial.printf("Wi-Fi Status Code: %d ", currentStatus);

    if(currentStatus == 6 && (currentTime - lastRetryTime > 7000))
    {
      lastRetryTime = currentTime;
      logInfo("Re-issuing fresh Wi-Fi connect request");
      WiFi.begin(WIFISSID, WIFIPASSWORD);
    }
  }
}

String getFormattedTimestamp() 
{
  struct tm timeinfo;

  if (!getLocalTime(&timeinfo)) 
  {
    Serial.println("Failed to obtain local time from internal clock");
    return "";
  }

  char buffer[32];
  strftime(buffer, sizeof(buffer), "%Y%m%d%H%M%S", &timeinfo);

  return String(buffer);
}
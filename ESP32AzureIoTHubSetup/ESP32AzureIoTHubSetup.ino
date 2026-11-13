#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <time.h>
#include <algorithm>
#include "FS.h"
#include "LittleFS.h"

#include <config.h>
#include <secretconfig.h>

const char* WIFISSID = WIFI_SSID;
const char* WIFIPASSWORD = WIFI_PASSWORD;

const char* IOTHUBHOST = IOT_HUB_HOST;
const char* DEVICEID = DEVICE_ID;
const char* DEVICEKEY = DEVICE_KEY;
const uint32_t PUBLISH_INTERVAL_MS = IOTHUB_INTERVAL;
const uint32_t SAS_TTL_S = 3600;
const long TIME_OFFSET_S = 19800;

constexpr uint32_t BAUDRATE = 115200;
constexpr size_t MAX_VALUES = 32;
constexpr size_t RX_LINE_LEN = 512;

static const char AZURE_ROOT_CA[] = AZURE_ROOT_CA_PEM;

WiFiClientSecure tls;
PubSubClient mqtt(tls);

String mqttUser, telemetryTopic;
uint32_t tokenExpiry = 0;

char   lineBuffer[RX_LINE_LEN];
size_t lineLength = 0;
bool   inFrame = false;

double   rxValues[MAX_VALUES];
size_t   rxCount    = 0;
bool     rxFresh    = false;
uint32_t validFrames   = 0;
uint32_t invalidFrames  = 0;
uint32_t lastPublishMs = 0;
bool timeSynced = false;
static uint32_t lastPrintTime = 0;
static uint32_t lastRetryTime = 0;
bool fsMounted = false;

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

int hexValue(char c) 
{
  if (c >= '0' && c <= '9') 
    return c - '0';
  if (c >= 'A' && c <= 'F') 
    return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') 
    return c - 'a' + 10;
  return -1;
}

bool parseFrame(char* s, size_t len, double* out, size_t maxOut, size_t& count) 
{
  if (len && s[len - 1] == '\r') 
    s[--len] = '\0';

  char* star = strchr(s, '*');

  if (!star || (size_t)(s + len - star) != 3) 
    return false;

  int high = hexValue(star[1]), low = hexValue(star[2]);

  if (high < 0 || low < 0) 
    return false;

  uint8_t cs = 0;

  for (char* p = s; p < star; p++) 
    cs ^= (uint8_t)*p;

  if (cs != (uint8_t)((high << 4) | low)) 
    return false;

  *star = '\0';
  count = 0;
  char* p = s;

  while (true) 
  {
    if (count >= maxOut) 
      return false;

    char* end;
    double d = strtod(p, &end);

    if (end == p) 
      return false;

    out[count++] = d;

    if (*end == ',') 
      p = end + 1;
    else if (*end == '\0') 
      break;
    else 
      return false;
  }

  return true;
}

void onReceiveValues(const double* v, size_t n) 
{
  Serial.printf("Received values(%u):", (unsigned)n);

  for (size_t i = 0; i < n; i++) 
    Serial.printf(" %.4f", v[i]);

  Serial.printf(" | Valid=%lu Invalid=%lu\n", (unsigned long)validFrames, (unsigned long)invalidFrames);
}

void pollLink() 
{
  size_t maxReads = 128; 
  while (Serial1.available() && maxReads-- > 0) 
  {
    char c = (char)Serial1.read();

    if (c == '$') 
    { 
      inFrame = true; 
      lineLength = 0; 
      continue; 
    }

    if (!inFrame) 
      continue;

    if (c == '\n') 
    {
      inFrame = false;
      lineBuffer[lineLength] = '\0';
      double tmp[MAX_VALUES];
      size_t n = 0;

      if (parseFrame(lineBuffer, lineLength, tmp, MAX_VALUES, n)) 
      {
        memcpy(rxValues, tmp, n * sizeof(double));
        rxCount = n;
        rxFresh = true;
        validFrames++;
        onReceiveValues(rxValues, rxCount);
      } 
      else 
      {
        invalidFrames++;
      }

      continue;
    }

    if (lineLength >= RX_LINE_LEN - 1) 
    {  
      inFrame = false; 
      invalidFrames++; 
      continue; 
    }

    lineBuffer[lineLength++] = c;
  }
}

void waitPolling(uint32_t ms) 
{
  uint32_t start = millis();
  while (millis() - start < ms) 
  {
    pollLink();
    delay(5);
  }
}

String encodeUrl(const String& in) 
{
  const char* hex = "0123456789ABCDEF";
  String out;

  for (size_t i = 0; i < in.length(); i++) 
  {
    char c = in[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~')
    {
      out += c;
    }
    else 
    {
      out += '%'; 
      out += hex[(c >> 4) & 0xF]; 
      out += hex[c & 0xF];
    }
  }

  return out;
}

bool createSasToken(uint32_t expiry, String& token) 
{
  uint8_t key[64]; 
  size_t keyLen = 0;

  if (mbedtls_base64_decode(key, sizeof(key), &keyLen, (const uint8_t*)DEVICEKEY, strlen(DEVICEKEY)) != 0)
    return false;

  String resource = encodeUrl(String(IOTHUBHOST) + "/devices/" + DEVICEID);
  String toSign = resource + "\n" + String(expiry);

  uint8_t mac[32];

  if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, keyLen, (const uint8_t*)toSign.c_str(), toSign.length(), mac) != 0)
    return false;

  uint8_t sig[64]; 
  size_t sigLen = 0;

  if (mbedtls_base64_encode(sig, sizeof(sig), &sigLen, mac, sizeof(mac)) != 0)
    return false;

  token = "SharedAccessSignature sr=" + resource + "&sig=" + encodeUrl(String((const char*)sig)) + "&se="  + String(expiry);
  return true;
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
      logInfo("[Wi-Fi] Re-issuing fresh connect request");
      WiFi.begin(WIFISSID, WIFIPASSWORD);
    }
  }
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
    Serial.printf("Current time: %s\n", timestamp);
  }
}

bool connectAzure() 
{
  uint32_t expiry = (uint32_t)time(nullptr) + SAS_TTL_S;
  String sas;

  if (!createSasToken(expiry, sas)) 
  {
    logError("Azure: SAS generation failed.");
    return false;
  }

  String iotHubHost = String(IOTHUBHOST);
  iotHubHost.toLowerCase();

  String deviceId = String(DEVICEID);
  deviceId.toLowerCase();

  mqttUser = iotHubHost + String("/") + deviceId + "/?api-version=2021-04-12";

  Serial.println(String("Azure: connecting to ...") + iotHubHost);
  if (!mqtt.connect(DEVICEID, mqttUser.c_str(), sas.c_str())) 
  {
    logError("Failed to connect to IoT Hub, state= "  + String(mqtt.state()));
    return false;
  }
  
  tokenExpiry = expiry;
  return true;
}

void ensureAzure() 
{
  if (!timeSynced) 
    return;

  if (mqtt.connected() && (uint32_t)time(nullptr) + 300 >= tokenExpiry) 
  {
    logInfo("Azure: renewing SAS token");
    mqtt.disconnect();
  }

  if (!mqtt.connected()) 
  {
    static uint32_t lastConnectAttempt = 0;
    if (millis() - lastConnectAttempt > 10000) 
    {
      lastConnectAttempt = millis();
      connectAzure();
    }
  }
}

void formatTimestamp(char* out, size_t cap) 
{
  time_t now = time(nullptr) + TIME_OFFSET_S;
  struct tm t;
  gmtime_r(&now, &t);
  strftime(out, cap, "%Y%m%d%H%M%S", &t);
}

size_t buildPayload(char* buf, size_t cap, const double* v, size_t n) 
{
  char ts[15];
  formatTimestamp(ts, sizeof(ts));
  int len = snprintf(buf, cap, "{\"deviceId\":\"%s\",\"ts\":\"%s\",\"values\":[", DEVICEID, ts);

  for (size_t i = 0; i < n && len < (int)cap; i++)
    len += snprintf(buf + len, cap - len, "%s%.4f", i ? "," : "", v[i]);

  if (len < (int)cap)
    len += snprintf(buf + len, cap - len, "]}");

  return (len < (int)cap) ? (size_t)len : 0;
}

void publishValues() 
{
  if (rxCount == 0) 
  {
    logInfo("[PUB] nothing received from S3 yet - skipped");
    return;
  }

  if (!rxFresh)
  {
    logInfo("[PUB] no new data since last publish - sending last values");
  }

  char payload[768];
  size_t len = buildPayload(payload, sizeof(payload), rxValues, rxCount);

  if (len == 0) 
  {
    logInfo("Payload too large for buffer");
    return;
  }

  bool publish = mqtt.publish(telemetryTopic.c_str(), (const uint8_t*)payload, len, false);
  Serial.printf("[%s] %s\n", publish ? "SENT" : "FAIL", payload);
  
  if (publish)
  {
    rxFresh = false;
    logInfo("Successfully published to IoT Hub");
  }
}

void setup() 
{
  logInfo("Beginning ESP32 process");

  logInfo("Serial initialization started");

  Serial.begin(115200);
  delay(500);

  if (LittleFS.begin(true)) 
  {
    fsMounted = true;
    logInfo("LittleFS mounted successfully");
  } 
  else 
  {
    Serial.println("[ERROR] LittleFS mount failed!");
  }

  Serial1.begin(BAUDRATE, SERIAL_8N1, 25, 26);
  Serial1.setRxBufferSize(1024);

  logInfo("Serial initialization ended");

  logInfo("Wi-Fi initialization started");

  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFISSID, WIFIPASSWORD);

  logInfo("Wi-Fi initialization ended");

  logInfo("Azure IoT Hub initialization started");

  tls.setCACert(AZURE_ROOT_CA);
  mqtt.setServer(IOTHUBHOST, 8883);
  mqtt.setBufferSize(2048); 
  mqtt.setKeepAlive(60);

  mqttUser = String(IOTHUBHOST) + "/" + DEVICEID + "/?api-version=2021-04-12";
  telemetryTopic = String("devices/") + DEVICEID + "/messages/events/$.ct=application%2Fjson&$.ce=utf-8";

  logInfo("Azure IoT Hub initialization ended");

  lastPublishMs = millis();
}

void loop() 
{
  pollLink();

  handleWiFi();

  handleNTP();

  if (WiFi.status() == WL_CONNECTED && timeSynced) 
  {
     ensureAzure();
     mqtt.loop();
     
     if (millis() - lastPublishMs >= PUBLISH_INTERVAL_MS) 
     {
       lastPublishMs = millis();
       publishValues();
     }
  }

  yield();
}
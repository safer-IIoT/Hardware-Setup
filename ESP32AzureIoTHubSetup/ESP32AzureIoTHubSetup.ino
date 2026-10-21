#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <time.h>
#include <algorithm>
#include <config.h>
#include <secretconfig.h>

const char* WIFISSID     = WIFI_SSID;
const char* WIFIPASSWORD = WIFI_PASSWORD;

const char* IOTHUBHOST = IOT_HUB_HOST;
const char* DEVICEID = DEVICE_ID;
const char* DEVICEKEY = DEVICE_KEY;

const uint32_t PUBLISH_INTERVAL_MS = 15000;
const uint32_t SAS_TTL_S           = 3600;    // token lifetime (renewed automatically)
const size_t   NUM_VALUES          = 15;
const long     TIME_OFFSET_S       = 0;

static const char AZURE_ROOT_CA[] = AZURE_ROOT_CA_PEM;

WiFiClientSecure tls;
PubSubClient mqtt(tls);

String mqttUser, telemetryTopic;
uint32_t tokenExpiry = 0;
uint32_t lastPublishMs = 0;

String urlEncode(const String& in) {
  const char* hex = "0123456789ABCDEF";
  String out;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') 
      out += c;
    else 
    { 
      out += '%'; out += hex[(c >> 4) & 0xF]; out += hex[c & 0xF]; 
    }
  }
  return out;
}

bool makeSasToken(uint32_t expiry, String& token) {
  uint8_t key[64]; size_t keyLen = 0;
  if (mbedtls_base64_decode(key, sizeof(key), &keyLen, (const uint8_t*)DEVICEKEY, strlen(DEVICEKEY)) != 0) 
    return false;

  String resource = urlEncode(String(IOTHUBHOST) + "/devices/" + DEVICEID);
  String toSign   = resource + "\n" + String(expiry);

  uint8_t mac[32];
  if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, keyLen, (const uint8_t*)toSign.c_str(), toSign.length(), mac) != 0) 
    return false;

  uint8_t sig[64]; size_t sigLen = 0;
  if (mbedtls_base64_encode(sig, sizeof(sig), &sigLen, mac, sizeof(mac)) != 0) 
    return false;

  token = "SharedAccessSignature sr=" + resource +
          "&sig=" + urlEncode(String((const char*)sig)) +
          "&se="  + String(expiry);
  return true;
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) 
    return;
  Serial.printf("Wi-Fi: connecting to %s", WIFISSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFISSID, WIFIPASSWORD);
  while (WiFi.status() != WL_CONNECTED) 
  { 
    delay(500); 
    Serial.print("."); 
  }
  Serial.printf("\nWi-Fi: connected, IP %s\n", WiFi.localIP().toString().c_str());
}

void syncTime() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  Serial.print("NTP: syncing");
  while (time(nullptr) < 1700000000) 
  { 
    delay(500); 
    Serial.print("."); 
  }
  Serial.println(" done");
}

bool connectAzure() {
  uint32_t expiry = (uint32_t)time(nullptr) + SAS_TTL_S;
  String sas;
  if (!makeSasToken(expiry, sas)) {
    Serial.println("Azure: SAS generation failed - check DEVICEKEY");
    return false;
  }
  Serial.printf("Azure: connecting to %s ... ", IOTHUBHOST);
  if (!mqtt.connect(DEVICEID, mqttUser.c_str(), sas.c_str())) {
    Serial.printf("failed, state=%d\n", mqtt.state());
    return false;
  }
  tokenExpiry = expiry;
  Serial.println("connected");
  return true;
}

void ensureAzure() {
  if (mqtt.connected() && (uint32_t)time(nullptr) + 300 >= tokenExpiry) {
    Serial.println("Azure: renewing SAS token");
    mqtt.disconnect();
  }
  uint32_t backoff = 2000;
  while (!mqtt.connected()) {
    connectWiFi();
    if (connectAzure()) 
      break;
    delay(backoff);
    backoff = std::min<uint32_t>(backoff * 2, 60000);
  }
}

void readValues(double* v, size_t n) {
  for (size_t i = 0; i < n; i++) v[i] = 20.0 + random(0, 10000) / 1000.0;
}

void formatTimestamp(char* out, size_t cap) {
  time_t now = time(nullptr) + TIME_OFFSET_S;
  struct tm t;
  gmtime_r(&now, &t);
  strftime(out, cap, "%Y%m%d%H%M%S", &t);
}

size_t buildPayload(char* buf, size_t cap, const double* v, size_t n) {
  char ts[15];
  formatTimestamp(ts, sizeof(ts));
  int len = snprintf(buf, cap, "{\"deviceId\":\"%s\",\"ts\":\"%s\",\"values\":[",
                     DEVICEID, ts);
  for (size_t i = 0; i < n && len < (int)cap; i++)
    len += snprintf(buf + len, cap - len, "%s%.4f", i ? "," : "", v[i]);
  if (len < (int)cap) 
    len += snprintf(buf + len, cap - len, "]}");
  return (len < (int)cap) ? (size_t)len : 0;   // 0 = buffer too small
}

void publishValues() {
  double values[NUM_VALUES];
  readValues(values, NUM_VALUES);

  char payload[512];
  size_t len = buildPayload(payload, sizeof(payload), values, NUM_VALUES);
  if (len == 0) 
  { 
    Serial.println("Payload too large for buffer");
    return; 
  }

  bool ok = mqtt.publish(telemetryTopic.c_str(), (const uint8_t*)payload, len, false);
  Serial.printf("[%s] %s\n", ok ? "SENT" : "FAIL", payload);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  connectWiFi();
  syncTime();

  tls.setCACert(AZURE_ROOT_CA);
  mqtt.setServer(IOTHUBHOST, 8883);
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(60);

  mqttUser = String(IOTHUBHOST) + "/" + DEVICEID + "/?api-version=2021-04-12";
  telemetryTopic = String("devices/") + DEVICEID +
                   "/messages/events/$.ct=application%2Fjson&$.ce=utf-8";

  ensureAzure();
  lastPublishMs = millis() - PUBLISH_INTERVAL_MS;
}

void loop() {
  ensureAzure();
  mqtt.loop();

  if (millis() - lastPublishMs >= PUBLISH_INTERVAL_MS) {
    lastPublishMs += PUBLISH_INTERVAL_MS;
    if (millis() - lastPublishMs >= PUBLISH_INTERVAL_MS)
      lastPublishMs = millis();
    publishValues();
  }
  delay(10);
}

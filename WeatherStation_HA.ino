/*
 * ESP8266 小气象站 v3.1 —— Home Assistant MQTT 自动发现版
 *
 * 硬件: NodeMCU + AHT20/BMP280 二合一模块 + 0.96" SSD1306 OLED (无新增硬件)
 * 接线: 模块 VDD/VCC->3V3  GND->G  SDA->D2(GPIO4)  SCL/SCK->D1(GPIO5)
 *
 * 功能:
 *   - 本地显示: FLASH 键轻按切换 主页/信息页,信息页 60s 自动跳回;长按 5s 重启(保持按住进配网)
 *   - 气压 3 小时趋势: 主页右上角箭头 + HA 实体 pressure_3h (每 5 分钟采样,40 点环形缓冲)
 *   - 露点计算 (Magnus 公式) → HA 实体 dewpoint
 *   - 板载 LED: 全正常=心跳微闪; WiFi 断=快闪; MQTT 断=慢闪
 *   - WiFi 配网: WiFiManager, 改 WiFi/MQTT 无需刷机; 开机按住 FLASH 进配网
 *   - HA MQTT 自动发现 6 实体, LWT 在线/离线, 20s 上报
 *   - I2C 50kHz (杜邦线实测必需), 传感器掉线 15s 自愈
 */

#include <ESP8266WiFi.h>
#include <EEPROM.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_BME280.h>

// ---------- I2C / 显示 ----------
#define SDA_PIN    4            // GPIO4 = D2
#define SCL_PIN    5            // GPIO5 = D1
#define SCREEN_W   128
#define SCREEN_H   64
#define REFRESH_MS 1000
#define RETRY_MS   15000

// ---------- 气压趋势 ----------
#define TREND_N        40       // 40 点 x 5 分钟 = 3.3 小时
#define TREND_SAMPLEMS 300000UL
#define RISING_TH      1.0f     // hPa, 3h 变化超过它算上升/下降

// ---------- MQTT ----------
#define MQTT_PORT      1883
#define PUBLISH_MS     20000
#define MQTT_RETRY_MS  5000

// ---------- EEPROM 配置 ----------
struct Cfg {
  uint32_t magic;
  char mqttServer[40];
  char mqttUser[24];
  char mqttPass[40];
};
const uint32_t CFG_MAGIC = 0x57535432;   // "WST2" v3.1

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
Adafruit_AHTX0   aht;
Adafruit_BMP280  bmp;
Adafruit_BME280  bme;
WiFiClient       wifiClient;
PubSubClient     mqtt(wifiClient);

bool     oledOK = false, ahtOK = false, bmpOK = false, usingBME = false;
bool     discoverySent = false, saveCfgFlag = false;
uint8_t  ahtBad = 0, bmpBad = 0;         // 读数连续异常计数(半通总线防护)
uint32_t lastRetry = 0, lastPub = 0, lastMqttTry = 0, lastTrendSample = 0, lastLoop = 0;
uint32_t infoPageUntil = 0;      // 信息页自动跳回主页的时间点,0=当前在主页
char     devId[16], apName[24];
Cfg      cfg;

// 气压趋势环形缓冲
float    pBuf[TREND_N];
int      pHead = 0, pCount = 0;

bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void loadCfg() {
  EEPROM.begin(sizeof(Cfg));
  EEPROM.get(0, cfg);
  if (cfg.magic != CFG_MAGIC) {
    strncpy(cfg.mqttServer, "192.168.100.14", sizeof(cfg.mqttServer) - 1);
    strncpy(cfg.mqttUser,   "weather",         sizeof(cfg.mqttUser)   - 1);
    strncpy(cfg.mqttPass,   "f770fccccf43",    sizeof(cfg.mqttPass)   - 1);
    cfg.magic = CFG_MAGIC;
    EEPROM.put(0, cfg);
    EEPROM.commit();
  }
}

void saveCfgCallback() { saveCfgFlag = true; }

void oledMsg(const char *l1, const char *l2 = "", const char *l3 = "") {
  if (!oledOK) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 8);
  display.println(l1);
  display.println(l2);
  display.println(l3);
  display.display();
}

/* ---------- OLED 初始化 ---------- */
void initOLED() {
  if (oledOK) return;
  uint8_t addr = 0;
  if      (i2cPresent(0x3C)) addr = 0x3C;
  else if (i2cPresent(0x3D)) addr = 0x3D;
  if (addr == 0) return;
  if (display.begin(SSD1306_SWITCHCAPVCC, addr)) {
    oledOK = true;
    oledMsg("Weather Station v3.1");
    Serial.println(F("OLED 就绪"));
  }
}

/* ---------- 传感器初始化 ---------- */
void initSensors() {
  if (!ahtOK && i2cPresent(0x38)) {
    ahtOK = aht.begin();
    if (ahtOK) Serial.println(F("AHT20 就绪"));
  }
  if (!bmpOK) {
    uint8_t addr = 0;
    if      (i2cPresent(0x76)) addr = 0x76;
    else if (i2cPresent(0x77)) addr = 0x77;
    if (addr == 0) return;
    usingBME = false;
    if (bmp.begin(addr)) {
      bmpOK = true;
      bmp.setSampling(Adafruit_BMP280::MODE_NORMAL, Adafruit_BMP280::SAMPLING_X2,
                      Adafruit_BMP280::SAMPLING_X16, Adafruit_BMP280::FILTER_X16,
                      Adafruit_BMP280::STANDBY_MS_500);
    } else if (bme.begin(addr)) {
      bmpOK = true; usingBME = true;
      bme.setSampling(Adafruit_BME280::MODE_NORMAL, Adafruit_BME280::SAMPLING_X2,
                      Adafruit_BME280::SAMPLING_X16, Adafruit_BME280::SAMPLING_X1,
                      Adafruit_BME280::FILTER_X16, Adafruit_BME280::STANDBY_MS_500);
    }
    if (bmpOK) Serial.printf("%s 就绪\n", usingBME ? "BME280" : "BMP280");
  }
}

/* ---------- 气压趋势 ---------- */
void trendPush(float p) {
  pBuf[pHead] = p;
  pHead = (pHead + 1) % TREND_N;
  if (pCount < TREND_N) pCount++;
}

float pressureDelta3h() {          // 当前 - 最早样本(不足 3h 时是现有跨度)
  if (pCount < 2) return NAN;
  int oldest = (pHead - pCount + TREND_N) % TREND_N;
  int newest = (pHead - 1 + TREND_N) % TREND_N;
  return pBuf[newest] - pBuf[oldest];
}

/* ---------- 露点 (Magnus) ---------- */
float dewPoint(float t, float rh) {
  if (isnan(t) || isnan(rh) || rh <= 0) return NAN;
  const float a = 17.62f, b = 243.12f;
  float g = logf(rh / 100.0f) + a * t / (b + t);
  return b * g / (a - g);
}

/* ---------- HA MQTT 自动发现(6 实体,带 unique_id) ---------- */
void mqttDiscovery() {
  char topic[80], payload[480];
  struct { const char *slug; const char *cls; const char *unit; const char *name; } s[6] = {
    {"temperature",  "temperature", "\xC2\xB0""C", "温度"},
    {"humidity",     "humidity",    "%",           "湿度"},
    {"pressure",     "pressure",    "hPa",         "气压"},
    {"chip_temp",    "temperature", "\xC2\xB0""C", "气压芯片温度"},
    {"dewpoint",     "temperature", "\xC2\xB0""C", "露点"},
    {"pressure_3h",  "",            "hPa",         "3小时气压变化"},
  };
  for (int i = 0; i < 6; i++) {
    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config", devId, s[i].slug);
    char uid[40], oid[48], devLow[16];
    snprintf(uid, sizeof(uid), "%s_%s", devId, s[i].slug);
    for (int j = 0; devId[j] && j < (int)sizeof(devLow) - 1; j++)
      devLow[j] = (devId[j] == '-') ? '_' : (char)tolower(devId[j]);
    devLow[strlen(devId)] = 0;
    snprintf(oid, sizeof(oid), "%s_%s", devLow, s[i].slug);
    if (s[i].cls[0])
      snprintf(payload, sizeof(payload),
        "{\"name\":\"%s\",\"unique_id\":\"%s\",\"object_id\":\"%s\","
        "\"device_class\":\"%s\",\"unit_of_measurement\":\"%s\","
        "\"state_topic\":\"%s/%s\",\"availability_topic\":\"%s/availability\","
        "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"ESP8266 气象站\",\"manufacturer\":\"DIY\",\"model\":\"NodeMCU ESP8266\"}}",
        s[i].name, uid, oid, s[i].cls, s[i].unit, devId, s[i].slug, devId, devId);
    else
      snprintf(payload, sizeof(payload),
        "{\"name\":\"%s\",\"unique_id\":\"%s\",\"object_id\":\"%s\","
        "\"unit_of_measurement\":\"%s\","
        "\"state_topic\":\"%s/%s\",\"availability_topic\":\"%s/availability\","
        "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"ESP8266 气象站\",\"manufacturer\":\"DIY\",\"model\":\"NodeMCU ESP8266\"}}",
        s[i].name, uid, oid, s[i].unit, devId, s[i].slug, devId, devId);
    mqtt.publish(topic, payload, true);
  }
}

void mqttConnect() {
  if (mqtt.connected()) return;
  if (millis() - lastMqttTry < MQTT_RETRY_MS) return;
  lastMqttTry = millis();
  char avail[40];
  snprintf(avail, sizeof(avail), "%s/availability", devId);
  if (mqtt.connect(devId, cfg.mqttUser, cfg.mqttPass, avail, 0, true, "offline")) {
    Serial.println(F("MQTT 已连接"));
    mqtt.publish(avail, "online", true);
    if (!discoverySent) { mqttDiscovery(); discoverySent = true; }
  } else {
    Serial.printf("MQTT 连接失败 rc=%d\n", mqtt.state());
  }
}

/* ---------- 板载 LED 状态 ---------- */
void ledUpdate(bool wifiDown, bool mqttDown) {
  uint32_t now = millis();
  bool on;
  if (wifiDown)      on = (now / 150) % 2;               // 快闪
  else if (mqttDown) on = (now / 600) % 2;               // 慢闪
  else               on = (now % 5000) < 60;             // 心跳微闪
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, on ? LOW : HIGH);            // 板载 LED 低电平点亮
}

/* ---------- 按键(轻按切页,长按 5s 重启重配网) ---------- */
void buttonUpdate() {
  static uint32_t pressStart = 0, lastEdge = 0;
  static bool wasDown = false;
  bool down = (digitalRead(0) == LOW);
  if (millis() - lastEdge < 50) return;                  // 消抖
  if (down != wasDown) {
    lastEdge = millis();
    if (down && !wasDown) pressStart = millis();
    if (!down && wasDown) {
      uint32_t hold = millis() - pressStart;
      if (hold > 50 && hold < 800) {                     // 轻按: 切页
        if (infoPageUntil == 0) { infoPageUntil = millis() + 60000UL; Serial.println(F("切页: 信息页 (60s后自动回主页)")); }
        else { infoPageUntil = 0; Serial.println(F("切页: 主页")); }
      }
    }
    wasDown = down;
  }
  if (down && wasDown && millis() - pressStart > 5000) { // 长按 5s: 重启
    Serial.println(F("长按 FLASH,重启进配网(保持按住)"));
    delay(200);
    ESP.restart();
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(50000);

  uint32_t chip = ESP.getChipId() & 0xFFFFFF;
  snprintf(devId, sizeof(devId), "ws-%06X", chip);
  snprintf(apName, sizeof(apName), "WeatherStation-%04X", (unsigned)(chip & 0xFFFF));

  pinMode(0, INPUT_PULLUP);
  for (int i = 0; i < TREND_N; i++) pBuf[i] = NAN;
  loadCfg();

  Serial.println();
  Serial.println(F("=== Weather Station v3.1 (HA MQTT) ==="));
  Serial.print(F("I2C 设备: "));
  bool none = true;
  for (uint8_t a = 1; a < 127; a++) if (i2cPresent(a)) { Serial.printf("0x%02X ", a); none = false; }
  if (none) Serial.print(F("(无! 检查接线)"));
  Serial.println();

  initOLED();
  initSensors();

  WiFiManager wm;
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(300);
  wm.setSaveConfigCallback(saveCfgCallback);
  WiFiManagerParameter p_server("server", "MQTT 服务器IP", cfg.mqttServer, 40);
  WiFiManagerParameter p_user("user",   "MQTT 用户名",   cfg.mqttUser,   24);
  WiFiManagerParameter p_pass("pass",   "MQTT 密码",     cfg.mqttPass,   40);
  wm.addParameter(&p_server);
  wm.addParameter(&p_user);
  wm.addParameter(&p_pass);

  if (digitalRead(0) == LOW) {
    Serial.println(F("检测到 FLASH 键,抹除已存 WiFi,重新配网"));
    wm.resetSettings();
    oledMsg("Config reset", "Release FLASH", "");
  }

  bool wifiOk = wm.autoConnect(apName, "12345678");
  if (!wifiOk) ESP.restart();

  if (saveCfgFlag) {
    strncpy(cfg.mqttServer, p_server.getValue(), sizeof(cfg.mqttServer) - 1);
    strncpy(cfg.mqttUser,   p_user.getValue(),   sizeof(cfg.mqttUser)   - 1);
    strncpy(cfg.mqttPass,   p_pass.getValue(),   sizeof(cfg.mqttPass)   - 1);
    EEPROM.put(0, cfg);
    EEPROM.commit();
    Serial.println(F("MQTT 配置已存 EEPROM"));
  }

  Serial.printf("WiFi 已连接: %s  IP: %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  oledMsg("WiFi OK", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());

  mqtt.setServer(cfg.mqttServer, MQTT_PORT);
  mqtt.setBufferSize(1024);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);  // 关 WiFi 休眠,规避局域网 TCP 静默死亡
  // 注意: 不要在未连接的 WiFiClient 上调 keepAlive() —— 本核心版本会空指针崩溃
}

void loop() {
  buttonUpdate();

  // 设备级自愈
  if (millis() - lastRetry > RETRY_MS) {
    lastRetry = millis();
    initOLED();
    initSensors();
  }

  mqttConnect();
  if (mqtt.connected()) mqtt.loop();     // PubSubClient 心跳(处理 ping/收包)

  // 1 秒一拍做读取/上报/显示;其余时间 10ms 快速轮询按键
  if (millis() - lastLoop < REFRESH_MS) {
    ledUpdate(WiFi.status() != WL_CONNECTED, !mqtt.connected());
    delay(10);
    return;
  }
  lastLoop = millis();

  // ---------- 读取 (含合理性门卫: 半通总线的垃圾值拦在本地,不进 HA) ----------
  float ahtTemp = NAN, ahtHumi = NAN;
  if (ahtOK) {
    sensors_event_t hum, temp;
    aht.getEvent(&hum, &temp);
    ahtTemp = temp.temperature;
    ahtHumi = hum.relative_humidity;
    if (isnan(ahtTemp) || ahtTemp < -40 || ahtTemp > 85 ||
        isnan(ahtHumi) || ahtHumi < 0 || ahtHumi > 100) {
      ahtTemp = NAN; ahtHumi = NAN;
      if (++ahtBad >= 5) {           // 连续5次异常: 判定芯片卡死,强制重新初始化
        ahtBad = 0; ahtOK = false;
        Serial.println(F("AHT20 读数异常,15秒内自动重新初始化"));
      }
    } else ahtBad = 0;
  }
  float pTemp = NAN, pres = NAN;
  if (bmpOK) {
    pTemp = usingBME ? bme.readTemperature() : bmp.readTemperature();
    pres  = (usingBME ? bme.readPressure() : bmp.readPressure()) / 100.0F;
    if (isnan(pTemp) || pTemp < -40 || pTemp > 85 ||
        isnan(pres) || pres < 300 || pres > 1200) {
      pTemp = NAN; pres = NAN;
      if (++bmpBad >= 5) {
        bmpBad = 0; bmpOK = false;
        pCount = 0;                  // 趋势缓冲可能已被污染,清空重攒
        Serial.println(F("气压传感器读数异常,15秒内自动重新初始化"));
      }
    } else bmpBad = 0;
  }

  // 气压趋势采样
  if (!isnan(pres) && millis() - lastTrendSample > TREND_SAMPLEMS) {
    lastTrendSample = millis();
    trendPush(pres);
  }

  float dew   = dewPoint(ahtTemp, ahtHumi);
  float p3h   = pressureDelta3h();

  // ---------- MQTT 上报 ----------
  if (mqtt.connected() && !isnan(ahtTemp) && millis() - lastPub > PUBLISH_MS) {
    lastPub = millis();
    char topic[64], val[16];
    bool ok = true;
    snprintf(topic, sizeof(topic), "%s/temperature", devId);
    ok = mqtt.publish(topic, dtostrf(ahtTemp, 1, 2, val)) && ok;
    snprintf(topic, sizeof(topic), "%s/humidity", devId);
    ok = mqtt.publish(topic, dtostrf(ahtHumi, 1, 1, val)) && ok;
    if (!isnan(pres)) {
      snprintf(topic, sizeof(topic), "%s/pressure", devId);
      ok = mqtt.publish(topic, dtostrf(pres, 1, 2, val)) && ok;
      snprintf(topic, sizeof(topic), "%s/chip_temp", devId);
      ok = mqtt.publish(topic, dtostrf(pTemp, 1, 2, val)) && ok;
      snprintf(topic, sizeof(topic), "%s/dewpoint", devId);
      ok = mqtt.publish(topic, dtostrf(dew, 1, 2, val)) && ok;
    }
    if (!isnan(p3h)) {
      snprintf(topic, sizeof(topic), "%s/pressure_3h", devId);
      ok = mqtt.publish(topic, dtostrf(p3h, 1, 2, val)) && ok;
    }
    if (!ok) {   // 任一条发布失败 = 连接已僵尸,立即重建
      Serial.println(F("MQTT 发布失败,重建连接"));
      mqtt.disconnect();
    }
  }

  // ---------- 串口 ----------
  char pb[16];
  if (ahtOK) Serial.printf("AHT20 : T=%.2f C RH=%.1f %% 露点=%.1f C\n", ahtTemp, ahtHumi, dew);
  if (bmpOK) Serial.printf("%s: T=%.2f C P=%.2f hPa 3h变化=%s\n", usingBME ? "BME280" : "BMP280",
                           pTemp, pres, isnan(p3h) ? "采样中" : dtostrf(p3h, 1, 2, pb));
  Serial.printf("WiFi:%s MQTT:%s 页面:%s\n",
    WiFi.status() == WL_CONNECTED ? "OK" : "断",
    mqtt.connected() ? "OK" : "断",
    infoPageUntil ? "信息" : "主页");
  Serial.println(F("--------------------"));

  // ---------- OLED ----------
  if (oledOK) {
    if (infoPageUntil && millis() > infoPageUntil) infoPageUntil = 0;   // 60s 自动回主页
    display.clearDisplay();
    if (infoPageUntil) {
      // ---- 信息页 (字号1, 仅 ASCII —— 经典字体无中文字形) ----
      display.setTextSize(1);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 0);
      if (!isnan(dew)) display.printf("Dew:%.1fC\n", dew);   else display.println(F("Dew: --"));
      if (!isnan(p3h)) display.printf("3h:%+.2fhPa %c\n", p3h,
                            p3h >= RISING_TH ? 24 : (p3h <= -RISING_TH ? 25 : 26));
      else             display.println(F("3h: sampling"));
      display.printf("AP: %s\n", WiFi.SSID().c_str());
      display.printf("IP: %s\n", WiFi.localIP().toString().c_str());
      uint32_t up = millis() / 1000;
      display.printf("Up: %luh%02lum\n", up / 3600, (up % 3600) / 60);
      display.printf("MQTT: %s\n", mqtt.connected() ? "OK" : "X");
    } else {
      // ---- 主页 (字号2) ----
      display.setTextSize(2);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 0);
      if (!isnan(ahtTemp)) display.printf("T:%.1fC\n", ahtTemp); else display.println(F("T: --"));
      if (!isnan(ahtHumi)) display.printf("H:%.1f%%\n", ahtHumi); else display.println(F("H: --"));
      if (!isnan(pres))    display.printf("P:%.0fhPa\n", pres);   else display.println(F("P: ---"));
      if (!isnan(pTemp))   display.printf("B:%.1fC\n", pTemp);
      else                 display.println(usingBME ? F("BME280") : F("BMP280!"));
      // 趋势箭头在温度行右侧
      if (pCount >= 2) {
        display.setCursor(114, 0);
        display.write(p3h >= RISING_TH ? 24 : (p3h <= -RISING_TH ? 25 : 26));   // ↑ ↓ →
      }
    }
    display.display();
  }

  // LED 状态灯
  ledUpdate(WiFi.status() != WL_CONNECTED, !mqtt.connected());

  delay(10);          // 非阻塞主循环: 按键 10ms 轮询一次
}

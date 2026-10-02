/*
 * ESP8266 (NodeMCU 1.0 / D1 mini) + AHT20 温湿度 + BMP280 气压 + SSD1306 OLED
 *
 * 三个模块全部并接到同一条 I2C 总线:
 *   SDA -> D2 (GPIO4)      SCL -> D1 (GPIO5)
 *   VCC -> 3V3             GND -> G
 *
 * 模块 I2C 地址(自动探测,不用改代码):
 *   AHT20  = 0x38 (固定)
 *   BMP280 = 0x76 (SDO接GND,最常见) 或 0x77 (SDO接VCC)
 *   OLED   = 0x3C (最常见) 或 0x3D
 *
 * 注意: 国产三合一紫色小板上标的"BMP280"有不少实际是 BME280 芯片,
 *       程序会自动识别并兜底,串口里会打印实际用的是哪个。
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_BME280.h>

#define SDA_PIN  4      // GPIO4 = 板上丝印 D2
#define SCL_PIN  5      // GPIO5 = 板上丝印 D1
#define SCREEN_W 128
#define SCREEN_H 64      // 0.96 寸屏是 64; 0.91 寸小屏请改成 32

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
Adafruit_AHTX0   aht;
Adafruit_BMP280  bmp;
Adafruit_BME280  bme;

bool ahtOK    = false;
bool bmpOK    = false;
bool usingBME = false;

bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void scanI2C() {
  Serial.print(F("I2C 总线上发现设备: "));
  bool none = true;
  for (uint8_t a = 1; a < 127; a++) {
    if (i2cPresent(a)) { Serial.printf("0x%02X ", a); none = false; }
  }
  if (none) Serial.print("(无!检查 SDA=D2 SCL=D1 接线和供电)");
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(50000);   // 杜邦线+面包板下 50kHz 比默认 100kHz 稳得多
  scanI2C();

  // OLED
  uint8_t oledAddr = i2cPresent(0x3C) ? 0x3C : 0x3D;
  if (display.begin(SSD1306_SWITCHCAPVCC, oledAddr)) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(F("Sensor starting..."));
    display.display();
  } else {
    Serial.println(F("OLED 未找到 (试过 0x3C/0x3D)"));
  }

  // AHT20
  ahtOK = i2cPresent(0x38) && aht.begin();
  Serial.println(ahtOK ? F("AHT20 OK") : F("AHT20 未找到 (0x38)"));

  // BMP280,认不出时按 BME280 再试一次
  uint8_t pAddr = i2cPresent(0x76) ? 0x76 : 0x77;
  if (i2cPresent(pAddr)) {
    if (bmp.begin(pAddr)) {
      bmpOK = true;
      bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                      Adafruit_BMP280::SAMPLING_X2,   // 温度过采样
                      Adafruit_BMP280::SAMPLING_X16,  // 气压过采样
                      Adafruit_BMP280::FILTER_X16,
                      Adafruit_BMP280::STANDBY_MS_500);
    } else if (bme.begin(pAddr)) {
      bmpOK = true;
      usingBME = true;
      bme.setSampling(Adafruit_BME280::MODE_NORMAL,
                      Adafruit_BME280::SAMPLING_X2,
                      Adafruit_BME280::SAMPLING_X16,
                      Adafruit_BME280::SAMPLING_X1,   // 湿度过采样(仅BME280有)
                      Adafruit_BME280::FILTER_X16,
                      Adafruit_BME280::STANDBY_MS_500);
    }
  }
  Serial.println(bmpOK
      ? (usingBME ? F("气压传感器: 实际是 BME280") : F("气压传感器: BMP280 OK"))
      : F("气压传感器未找到 (0x76/0x77)"));
}

void loop() {
  // ---------- 读取 AHT20 ----------
  float ahtTemp = NAN, ahtHumi = NAN;
  if (ahtOK) {
    sensors_event_t hum, temp;
    aht.getEvent(&hum, &temp);
    ahtTemp = temp.temperature;
    ahtHumi = hum.relative_humidity;
  }

  // ---------- 读取气压传感器 ----------
  float pTemp = NAN, pres = NAN;
  if (bmpOK) {
    if (usingBME) {
      pTemp = bme.readTemperature();
      pres  = bme.readPressure() / 100.0F;
    } else {
      pTemp = bmp.readTemperature();
      pres  = bmp.readPressure() / 100.0F;
    }
  }

  // ---------- 串口输出 ----------
  if (ahtOK)   Serial.printf("AHT20 : T=%.2f C  RH=%.1f %%\n", ahtTemp, ahtHumi);
  if (bmpOK)   Serial.printf("%s: T=%.2f C  P=%.2f hPa\n", usingBME ? "BME280" : "BMP280", pTemp, pres);
  Serial.println("--------------------");

  // ---------- OLED 显示 (128x64, 4 行, 字号2) ----------
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);

  if (!isnan(ahtTemp)) display.printf("T:%.1fC\n", ahtTemp);
  else                 display.println(F("T: --"));

  if (!isnan(ahtHumi)) display.printf("H:%.1f%%\n", ahtHumi);
  else                 display.println(F("H: --"));

  if (!isnan(pres))    display.printf("P:%.0fhPa\n", pres);
  else                 display.println(F("P: ---"));

  if (!usingBME && !isnan(pTemp)) display.printf("B:%.1fC\n", pTemp);
  else if (usingBME)              display.println(F("BME280"));
  else                            display.println(F("BMP280!"));

  display.display();
  delay(1000);
}

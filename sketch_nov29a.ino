#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

#define SDA_PIN 4   // D2
#define SCL_PIN 5   // D1
#define AHT_ADDR 0x38

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(
  U8G2_R0,
  U8X8_PIN_NONE,
  SCL_PIN,
  SDA_PIN
);

// 触发 AHT30 测量
bool triggerAHT() {
  Wire.beginTransmission(AHT_ADDR);
  Wire.write(0xAC);
  Wire.write(0x33);
  Wire.write(0x00);
  return (Wire.endTransmission() == 0);
}

// 读取 AHT30 原始数据并解析
bool readAHT(float &temperature, float &humidity) {
  if (!triggerAHT()) return false;
  delay(80); // AHT30 需要 ~75ms

  Wire.requestFrom(AHT_ADDR, (uint8_t)6);
  if (Wire.available() < 6) return false;

  uint8_t buf[6];
  for (int i = 0; i < 6; i++) buf[i] = Wire.read();

  uint32_t hum_raw =
      ((uint32_t)buf[1] << 12) |
      ((uint32_t)buf[2] << 4) |
      ((uint32_t)buf[3] >> 4);

  humidity = (hum_raw / 1048576.0) * 100.0;

  uint32_t temp_raw =
      (((uint32_t)(buf[3] & 0x0F)) << 16) |
      ((uint32_t)buf[4] << 8) |
      buf[5];

  temperature = (temp_raw / 1048576.0) * 200.0 - 50.0;

  return true;
}

void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);

  u8g2.begin();
  u8g2.enableUTF8Print();
}

void loop() {
  float temp, hum;

  if (readAHT(temp, hum)) {
    Serial.printf("T=%.2f  H=%.2f\n", temp, hum);
  } else {
    Serial.println("AHT read failed!");
  }

  // OLED 显示
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_fub14_tr);

  u8g2.setCursor(0, 24);
  u8g2.printf("Temp: %.1f C", temp);

  u8g2.setCursor(0, 54);
  u8g2.printf("Humi: %.1f %%", hum);

  u8g2.sendBuffer();
  delay(1000);
}

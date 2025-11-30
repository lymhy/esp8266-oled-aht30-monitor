#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <DNSServer.h>

#define SDA_PIN 4   // D2
#define SCL_PIN 5   // D1
#define AHT_ADDR 0x38

// OLED 显示
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(
  U8G2_R0,
  U8X8_PIN_NONE,
  SCL_PIN,
  SDA_PIN
);

// Web 服务器
ESP8266WebServer server(80);

// WiFiManager
WiFiManager wifiManager;

// 全局变量
float currentTemperature = 0.0;
float currentHumidity = 0.0;
bool broadcastEnabled = false;
String broadcastMessage = "环境监测广播";
unsigned long lastBroadcastTime = 0;
const unsigned long BROADCAST_INTERVAL = 15 * 60 * 1000; // 15分钟

// 显示状态控制
unsigned long displayStartTime = 0;
enum DisplayState {
  DISPLAY_WIFI_CONFIG,    // 显示配网信息
  DISPLAY_NORMAL          // 正常显示温湿度
};
DisplayState currentDisplayState = DISPLAY_WIFI_CONFIG;
const unsigned long WIFI_CONFIG_DISPLAY_TIME = 10000; // 配网信息显示10秒

// 触发 AHT30 测量
bool triggerAHT() {
  Wire.beginTransmission(AHT_ADDR);
  Wire.write(0xAC);
  Wire.write(0x33);
  Wire.write(0x00);
  return (Wire.endTransmission() == 0);
}

// 读取 AHT30 数据
bool readAHT(float &temperature, float &humidity) {
  if (!triggerAHT()) return false;
  delay(80);

  uint8_t bytesReceived = Wire.requestFrom((uint8_t)AHT_ADDR, (uint8_t)6);
  if (bytesReceived < 6) return false;

  uint8_t buf[6];
  for (int i = 0; i < 6; i++) buf[i] = Wire.read();

  if ((buf[0] & 0x68) != 0x08) return false;

  uint32_t hum_raw = ((uint32_t)buf[1] << 12) | ((uint32_t)buf[2] << 4) | ((uint32_t)buf[3] >> 4);
  humidity = (hum_raw / 1048576.0) * 100.0;

  uint32_t temp_raw = (((uint32_t)(buf[3] & 0x0F)) << 16) | ((uint32_t)buf[4] << 8) | buf[5];
  temperature = (temp_raw / 1048576.0) * 200.0 - 50.0;

  if (humidity > 100.0 || humidity < 0.0 || temperature > 85.0 || temperature < -40.0) {
    return false;
  }

  return true;
}

// HTML 页面
const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ESP8266 智能气象站</title>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <style>
        body {
            font-family: Arial, sans-serif;
            margin: 20px;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
        }
        .container {
            max-width: 800px;
            margin: 0 auto;
            background: rgba(255,255,255,0.1);
            padding: 20px;
            border-radius: 15px;
            backdrop-filter: blur(10px);
        }
        .data-card {
            background: rgba(255,255,255,0.2);
            padding: 20px;
            border-radius: 10px;
            margin: 20px 0;
            text-align: center;
        }
        .temp {
            font-size: 3em;
            font-weight: bold;
        }
        .humidity {
            font-size: 2em;
            margin-top: 10px;
        }
        .controls {
            background: rgba(255,255,255,0.15);
            padding: 20px;
            border-radius: 10px;
            margin: 20px 0;
        }
        input, textarea, button, select {
            width: 100%;
            padding: 12px;
            margin: 8px 0;
            border: none;
            border-radius: 8px;
            box-sizing: border-box;
            font-size: 16px;
        }
        button {
            background: #007bff;
            color: white;
            cursor: pointer;
            transition: background 0.3s;
        }
        button:hover {
            background: #0056b3;
        }
        .wifi-btn {
            background: #28a745;
        }
        .wifi-btn:hover {
            background: #1e7e34;
        }
        .status {
            padding: 10px;
            border-radius: 5px;
            margin: 10px 0;
            text-align: center;
        }
        .on { background: rgba(40, 167, 69, 0.3); }
        .off { background: rgba(220, 53, 69, 0.3); }
        .info-card {
            background: rgba(255, 193, 7, 0.2);
            padding: 15px;
            border-radius: 8px;
            margin: 15px 0;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>🌡️ 智能气象站监控系统</h1>
        
        <div class="data-card">
            <div class="temp" id="temperature">--.- °C</div>
            <div class="humidity" id="humidity">--.- %</div>
            <div>更新时间: <span id="updateTime">--:--:--</span></div>
            <div>设备IP: <span id="deviceIP">--.--.--.--</span></div>
        </div>

        <div class="info-card">
            <strong>📡 网络状态:</strong> <span id="wifiStatus">连接中...</span><br>
            <strong>🕒 广播间隔:</strong> 15分钟
        </div>

        <div class="controls">
            <h2>📢 局域网广播设置</h2>
            
            <div class="status off" id="broadcastStatus">
                广播状态: 关闭
            </div>

            <textarea id="broadcastMessage" rows="3" placeholder="输入广播消息...">环境监测广播</textarea>
            
            <button onclick="toggleBroadcast()" id="broadcastBtn">开启广播</button>
            <button onclick="updateData()">🔄 刷新数据</button>
            <button onclick="switchToAPMode()" class="wifi-btn">📶 切换WiFi网络</button>
        </div>
    </div>

    <script>
        let autoRefreshInterval;
        
        function updateData() {
            fetch('/data')
                .then(response => response.json())
                .then(data => {
                    document.getElementById('temperature').textContent = data.temperature.toFixed(1) + ' °C';
                    document.getElementById('humidity').textContent = data.humidity.toFixed(1) + ' %';
                    document.getElementById('updateTime').textContent = new Date().toLocaleTimeString();
                    document.getElementById('deviceIP').textContent = data.deviceIP;
                    document.getElementById('wifiStatus').textContent = data.wifiStatus;
                    
                    const statusDiv = document.getElementById('broadcastStatus');
                    const button = document.getElementById('broadcastBtn');
                    
                    if (data.broadcastEnabled) {
                        statusDiv.innerHTML = '广播状态: <strong>开启</strong> - 消息: ' + data.broadcastMessage;
                        statusDiv.className = 'status on';
                        button.textContent = '关闭广播';
                    } else {
                        statusDiv.innerHTML = '广播状态: 关闭';
                        statusDiv.className = 'status off';
                        button.textContent = '开启广播';
                    }
                })
                .catch(error => {
                    console.error('获取数据失败:', error);
                    document.getElementById('wifiStatus').textContent = '连接失败';
                });
        }

        function toggleBroadcast() {
            const message = document.getElementById('broadcastMessage').value;
            const enabled = document.getElementById('broadcastBtn').textContent === '开启广播';
            
            fetch('/broadcast', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: 'enabled=' + enabled + '&message=' + encodeURIComponent(message)
            })
            .then(response => response.json())
            .then(data => {
                if (data.success) {
                    updateData();
                    alert(enabled ? '广播已开启！每15分钟发送一次。' : '广播已关闭！');
                }
            });
        }

        function switchToAPMode() {
            if (confirm('确定要切换WiFi网络吗？设备将重启进入配网模式。')) {
                fetch('/switchwifi', {method: 'POST'})
                .then(response => response.json())
                .then(data => {
                    if (data.success) {
                        alert('设备正在重启进入配网模式，请重新连接WiFi AP: ESP8266_Weather');
                        setTimeout(() => {
                            window.location.href = 'http://192.168.4.1';
                        }, 3000);
                    }
                });
            }
        }

        // 自动刷新
        document.addEventListener('DOMContentLoaded', function() {
            updateData();
            autoRefreshInterval = setInterval(updateData, 5000);
        });
    </script>
</body>
</html>
)rawliteral";

// 更新OLED显示
void updateOLEDDisplay() {
  u8g2.clearBuffer();
  
  if (currentDisplayState == DISPLAY_WIFI_CONFIG) {
    // 显示配网信息（大字体，清晰可见）
    u8g2.setFont(u8g2_font_helvB12_tr);
    u8g2.drawStr(0, 20, "请配置WiFi");
    u8g2.drawStr(0, 40, "连接:ESP8266_Weather");
    u8g2.drawStr(0, 60, "IP:192.168.4.1");
    
    // 检查是否超时，切换到正常显示
    if (millis() - displayStartTime > WIFI_CONFIG_DISPLAY_TIME) {
      currentDisplayState = DISPLAY_NORMAL;
    }
  } else {
    // 正常显示温湿度（大字体，清晰布局）
    u8g2.setFont(u8g2_font_fub20_tr); // 使用更大的字体
    
    // 温度显示
    u8g2.setCursor(0, 30);
    if (currentTemperature > -40.0 && currentTemperature < 85.0) {
      u8g2.printf("%.1fC", currentTemperature);
    } else {
      u8g2.print("---C");
    }
    
    // 湿度显示
    u8g2.setCursor(0, 60);
    if (currentHumidity >= 0.0 && currentHumidity <= 100.0) {
      u8g2.printf("%.1f%%", currentHumidity);
    } else {
      u8g2.print("---%");
    }
    
    // 只在底部显示小字状态信息
    u8g2.setFont(u8g2_font_6x10_tr);
    if (WiFi.status() == WL_CONNECTED) {
      u8g2.setCursor(0, 12);
      u8g2.print("已联网");
      if (broadcastEnabled) {
        u8g2.setCursor(70, 12);
        u8g2.print("广播开");
      }
    } else {
      u8g2.setCursor(0, 12);
      u8g2.print("配网模式");
    }
  }
  
  u8g2.sendBuffer();
}

// Web服务器路由处理
void handleRoot() {
  server.send(200, "text/html", htmlPage);
}

void handleData() {
  String json = "{";
  json += "\"temperature\":" + String(currentTemperature, 1);
  json += ",\"humidity\":" + String(currentHumidity, 1);
  json += ",\"broadcastEnabled\":" + String(broadcastEnabled ? "true" : "false");
  json += ",\"broadcastMessage\":\"" + broadcastMessage + "\"";
  json += ",\"deviceIP\":\"" + WiFi.localIP().toString() + "\"";
  
  json += ",\"wifiStatus\":\"";
  if (WiFi.status() == WL_CONNECTED) {
    json += "已连接 (" + WiFi.SSID() + ")";
  } else {
    json += "配网模式";
  }
  json += "\"";
  
  json += "}";
  
  server.send(200, "application/json", json);
}

void handleBroadcast() {
  if (server.method() == HTTP_POST) {
    broadcastEnabled = server.arg("enabled") == "true";
    broadcastMessage = server.arg("message");
    
    String json = "{\"success\":true}";
    server.send(200, "application/json", json);
  }
}

void handleSwitchWiFi() {
  // 清除WiFi配置并重启进入配网模式
  wifiManager.resetSettings();
  String json = "{\"success\":true}";
  server.send(200, "application/json", json);
  
  delay(1000);
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);
  
  // 初始化OLED
  u8g2.begin();
  u8g2.enableUTF8Print();
  
  // 显示启动信息
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB12_tr);
  u8g2.drawStr(0, 30, "气象站启动");
  u8g2.drawStr(0, 50, "初始化中...");
  u8g2.sendBuffer();
  
  delay(2000);
  
  // 设置WiFiManager
  wifiManager.setConfigPortalTimeout(180); // 3分钟配网超时
  wifiManager.setAPCallback([](WiFiManager *myWiFiManager) {
    Serial.println("进入配网模式");
    Serial.println("AP名称: " + myWiFiManager->getConfigPortalSSID());
    Serial.println("AP IP: 192.168.4.1");
    
    // 开始显示配网信息
    currentDisplayState = DISPLAY_WIFI_CONFIG;
    displayStartTime = millis();
  });
  
  // 尝试连接WiFi，如果失败则启动配网模式
  if (!wifiManager.autoConnect("ESP8266_Weather", "12345678")) {
    Serial.println("配网超时，重启设备");
    delay(3000);
    ESP.restart();
  }
  
  // 连接成功
  Serial.println("WiFi连接成功!");
  Serial.println("IP地址: " + WiFi.localIP().toString());
  currentDisplayState = DISPLAY_NORMAL;
  
  // 设置Web服务器路由
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/broadcast", handleBroadcast);
  server.on("/switchwifi", handleSwitchWiFi);
  
  server.begin();
  Serial.println("HTTP服务器已启动");
}

void loop() {
  // 处理Web请求
  server.handleClient();
  
  // 读取传感器数据（不受网络模式影响）
  static unsigned long lastSensorRead = 0;
  if (millis() - lastSensorRead >= 2000) {
    lastSensorRead = millis();
    
    if (readAHT(currentTemperature, currentHumidity)) {
      Serial.printf("温度: %.1f°C, 湿度: %.1f%%\n", currentTemperature, currentHumidity);
    } else {
      Serial.println("AHT30 读取失败!");
    }
  }
  
  // 处理广播发送
  if (broadcastEnabled && WiFi.status() == WL_CONNECTED) {
    if (millis() - lastBroadcastTime >= BROADCAST_INTERVAL) {
      lastBroadcastTime = millis();
      // 这里可以添加实际的广播发送逻辑
      Serial.println("广播发送: " + broadcastMessage);
      Serial.println("发送到IP: " + WiFi.localIP().toString());
    }
  }
  
  // 更新OLED显示
  updateOLEDDisplay();
  
  delay(100);
}
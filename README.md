# ESP8266 小气象站（Home Assistant 版）

NodeMCU(ESP-12F) + AHT20/BMP280 二合一温湿度气压模块 + 0.96" SSD1306 OLED，
通过 MQTT 自动发现接入 Home Assistant，无需任何手工配置实体。

![架构](https://img.shields.io/badge/ESP8266-MQTT-blue) ![HA](https://img.shields.io/badge/Home%20Assistant-自动发现-green)

```
AHT20/BMP280 ──I2C──┐
                    │  50kHz
SSD1306 OLED ──I2C──┤
                    │
NodeMCU(ESP-12F) ───┴─ WiFi ──> Mosquitto(saserver:1883) ──> Home Assistant(:8123)
                                  自动发现 + 20秒上报 + LWT 在线离线
```

## 功能

| 功能 | 说明 |
|---|---|
| 温湿度（AHT20） | CRC8 校验，精度 ±0.3°C / ±2%RH |
| 气压 + 芯片温度（BMP280） | Bosch 补偿算法；实为 BME280 芯片时自动兜底 |
| 露点 | Magnus 公式由温湿度推导 |
| 气压 3 小时趋势 | 5 分钟采样 × 40 点环形缓冲，OLED 显示 ↑→↓ 箭头 |
| OLED 双页面 | 轻按 FLASH 切换主页/信息页，信息页 60s 自动跳回 |
| 板载 LED 状态灯 | 全正常=心跳微闪；WiFi 断=快闪；MQTT 断=慢闪 |
| WiFi 配网 | WiFiManager 热点配网，**改 WiFi 密码无需刷机** |
| HA 自动发现 | 6 实体带 unique_id，设备卡片 + LWT 在线/离线 |
| 自愈 | 传感器掉线 15s 自动重连；MQTT 断线 5s 重试；发布失败立即重建连接 |
| 合理性门卫 | 读数超物理范围（天文数字/负气压等）拦截在本地不上报；连续 5 次异常自动重新初始化芯片并清空气压趋势缓冲 |

## 接线（全部并到同一条 I2C 总线）

| 模块引脚 | NodeMCU / D1 mini |
|---|---|
| SDA | **D2**（GPIO4） |
| SCL / SCK | **D1**（GPIO5） |
| VCC / VDD | 3V3 |
| GND | G |

> 代码里写的是 GPIO 编号（4/5），选任何 ESP8266 板型都能编译；
> OLED 丝印是 SCK 也一样接 D1，四针 OLED 就是 I2C 屏。

## 快速开始

零基础请按顺序看 **[Arduino安装教程.md](Arduino安装教程.md)**（从装 IDE 到 HA 出数据，全程傻瓜式）。

熟手三步：

1. Arduino IDE 打开 `WeatherStation_HA.ino`，装好依赖库，选板型烧录；
2. 上电后手机连热点 `WeatherStation-XXXX`（密码 12345678）完成配网；
3. HA 里添加 MQTT 集成，等设备「ESP8266 气象站」自己出现。

## MQTT 主题与 HA 实体

| HA 实体 | MQTT 主题 | 单位 |
|---|---|---|
| sensor.xxx_wen_du（温度） | `ws-XXXXXX/temperature` | °C |
| sensor.xxx_shi_du（湿度） | `ws-XXXXXX/humidity` | % |
| sensor.xxx_qi_ya（气压） | `ws-XXXXXX/pressure` | hPa |
| sensor.xxx_qi_ya_xin_pian_wen_du | `ws-XXXXXX/chip_temp` | °C |
| sensor.xxx_lu_dian（露点） | `ws-XXXXXX/dewpoint` | °C |
| sensor.xxx_3_xiao_shi_qi_ya_bian_hua | `ws-XXXXXX/pressure_3h` | hPa |

- 发现主题：`homeassistant/sensor/ws-XXXXXX/<slug>/config`（retain）
- 在线状态：`ws-XXXXXX/availability`（online/offline，LWT）
- 上报周期：20 秒；数据 20s 一跳，HA 自动存历史

## 日常操作

| 操作 | 方式 |
|---|---|
| 切换 OLED 页面 | 轻按 FLASH 键（信息页 60s 自动回主页） |
| 改 WiFi 密码 | 不用管——连不上网自动重开热点；或长按 FLASH 5s 后重启继续保持按住 |
| 强制重新配网 | 上电前按住 FLASH 键插 USB |
| 看诊断日志 | 串口监视器 115200 |

## 代码顶部可调参数

```cpp
#define SDA_PIN    4     // 换引脚改这里
#define SCL_PIN    5
#define SCREEN_H   64    // 0.91寸小屏改 32
#define REFRESH_MS 1000  // 本地刷新
#define PUBLISH_MS 20000 // HA 上报周期
#define RETRY_MS   15000 // 掉线设备重连间隔
```

MQTT 服务器/账号在配网页面里改（存 EEPROM），或改代码里的默认值。

## 故障速查

| 症状 | 处理 |
|---|---|
| 串口一秒刷一行分隔线、无数据 | I2C 总线不通，检查接线（八成是接触不良） |
| 读数全是 0 / 182°C / 天文数字 | 传感器半脱线或总线半通：重插线 15 秒自愈；固件门卫已拦截垃圾值不进 HA，趋势缓冲自动清空 |
| COM 口出不来 | 换数据线 → 换主板后置 USB 口 → 装 CH340 驱动 |
| IDE 上传失败端口占用 | 关闭串口监视器再传 |
| OLED 中文乱码 | 经典字体无中文字形，属正常；要中文需迁 U8g2 |
| HA 搜不到设备 | 检查 HA 的 MQTT 集成是否已配置（新版 HA 只能 UI 配置） |

## 已知边界

- `pressure_3h` 上电 ~10 分钟才有第一个值，3 小时后才有预报意义；
- OLED 经典字体不支持中文（HA 网页端中文正常）；
- 固件依赖局域网内已部署 Mosquitto（部署命令见教程第 8 步）。

# RT4K ESP32-S3 IR Remote

一个基于 ESP32-S3 的红外遥控器项目，提供 Web 控制面板、AP 兜底、Wi-Fi Manager、状态可视化与 OTA 升级。
<img width="966" height="1598" alt="control" src="https://github.com/user-attachments/assets/e32b284b-aea8-4d43-9bf1-8bcf14ee6c08" />
<img width="982" height="1598" alt="Setting" src="https://github.com/user-attachments/assets/7047c5ab-5903-41a8-afb9-af97bfb1e0a0" />
<img width="940" height="1594" alt="wifi-manager" src="https://github.com/user-attachments/assets/782a17a1-7b7c-46e9-a18e-b23f5a0f5900" />

## 主要功能

- Web 遥控 UI（手机/电脑浏览器可用）
- 红外发射（IRremote）
- AP 兜底模式（默认 `RT4K-Remote`）
- Wi-Fi Manager（扫描、保存、忘记网络）
- Wi-Fi 状态与断连原因可视化
- OTA 无线升级（ArduinoOTA）
- LED 状态反馈与颜色控制

## 硬件与引脚

- 芯片：ESP32-S3
- 红外发射：`IR_SEND_PIN = 4`
- RGB LED：`RGB_LED_PIN = 48`

> 如果你的开发板引脚不同，请在 `rt4k-esp32-ir-remote.ino` 顶部修改宏定义。

## 依赖库

请在 Arduino IDE 中安装：

- `IRremote`
- `FastLED`

以下为 ESP32 Core 自带：

- `WiFi.h`
- `WebServer.h`
- `ESPmDNS.h`
- `ArduinoOTA.h`
- `Preferences.h`
- `esp_wifi.h`

## 快速开始

1. 在 `wifiList[]` 中填入你的 2.4G Wi-Fi（或留空只用 AP 模式）。
2. 编译并串口烧录 `rt4k-esp32-ir-remote.ino`。
3. 如果 STA 未连上，将自动开启 AP：
   - SSID: `RT4K-Remote`
   - Password: `ChangeMe123`
4. 浏览器打开：
   - AP 模式：`http://192.168.4.1`
   - STA 模式：设备 IP（`/status` 可查看）

## Wi-Fi Manager 用法

在 Settings 页点击 `Configure Wi-Fi`：

- `Scan` 扫描附近网络
- 选择或手动输入 SSID
- 输入密码后 `Save & Connect`
- `Forget` 清除保存的网络

## OTA 升级

1. 让电脑与 ESP32 在同一局域网（建议 STA 模式）。
2. Arduino IDE -> `Tools` -> `Port`，选择网络端口（如 `rt4k-remote`）。
3. 直接 Upload 即可 OTA。

## 常见问题

- `rt4k.local` 打不开：优先使用 IP 地址访问。
- iPhone 连接 AP 失败：先“忽略此网络”后重新输入密码。
- 能扫描到 Wi-Fi 但连不上：确认路由器允许 2.4G + WPA2/WPA2-WPA3 混合模式。

## 安全说明

- Wi-Fi 凭据仅保存在设备 NVS（本地），默认不上传云端。
- 建议首次使用后修改 AP 默认密码。

## License

MIT

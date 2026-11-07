# AI_BOT_speaker_transfer

Firmware **ESP32-WROOM** cho loa Bluetooth: đọc MP3 trong SPIFFS, giải mã và
stream qua A2DP tới loa/tai nghe Bluetooth.

Điều khiển được từ hai nguồn:

- **UART1 của WROOM** (TX=GPIO17, RX=GPIO16, 115200 8N1) — gõ lệnh
  (`play 1`, `pause`, `status`, ...) hoặc dùng mã `cm00..cm09`; số menu đơn
  chỉ để hiển thị, không phải lệnh.
- **Wi-Fi/TCP từ ESP32-S3 (AI_BOT)** — S3 kết nối router cho AI server đồng thời
  phát SoftAP để WROOM kết nối; trạng thái được gửi ngược qua TCP.

## Tài liệu

- [`docs/WIFI_TCP_CONTROL.md`](docs/WIFI_TCP_CONTROL.md) — kiến trúc nhận lệnh
  UART + Wi-Fi/TCP, cấu hình AP, giao thức, danh sách lệnh, xử lý sự cố.
- `AI_BOT/docs/WIFI_TCP_MUSIC_CONTROL.md` — phía ESP32-S3 (gửi lệnh, nhận
  status).

## Build / nạp

```powershell
pio run                 # build
pio run -t upload       # nạp firmware
tools\upload_fs.ps1     # nạp file .mp3 vào SPIFFS
```

> Project đặt `CONFIG_ESP_WIFI_IRAM_OPT=n` và `CONFIG_ESP_WIFI_RX_IRAM_OPT=n`
> trong `sdkconfig.defaults` để đủ IRAM khi chạy đồng thời Wi-Fi và
> Bluetooth Classic.

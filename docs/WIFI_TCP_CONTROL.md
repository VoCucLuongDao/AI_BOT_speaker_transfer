# Điều khiển WROOM qua Wi-Fi/TCP

ESP32-WROOM là Bluetooth A2DP Source và nhận lệnh từ ESP32-S3 (AI_BOT) qua
mạng Wi-Fi nội bộ của S3. Giao tiếp điều khiển không sử dụng ESP-NOW.

## Kết nối Wi-Fi

WROOM hoạt động ở chế độ Wi-Fi STA, tự kết nối SoftAP do S3 phát:

| Thiết lập | Giá trị |
|---|---|
| SSID | `AI_BOT` |
| Mật khẩu | `123456789` |
| Địa chỉ TCP server S3 | `192.168.4.1:3333` |
| Cấp phát địa chỉ WROOM | DHCP |

Thông tin này phải giống trong `src/bt_link.h` của hai project. S3 vẫn kết nối
router ở chế độ STA để liên lạc AI server; đồng thời chạy AP cho WROOM. AP dùng
cùng kênh radio với router nên không cần dò/chọn kênh thủ công. WROOM sẽ tự nối
lại Wi-Fi và TCP khi AP/router bị gián đoạn.

## Lệnh

Có thể gõ lệnh trên UART WROOM hoặc gửi từ UART S3. Các lệnh cơ bản dùng chung,
nhưng luồng chọn nguồn `cm02` khác nhau theo giao diện:

| Lệnh | Tác dụng |
|---|---|
| `cm00` | In menu |
| `cm01` | Quét thiết bị Bluetooth |
| `cm02` | UART WROOM: chọn loa và phát MP3 local; UART S3: chọn loa, nguồn và bài |
| `cm03` | Dừng phát hoặc ngắt A2DP |
| `cm04` | Liệt kê file MP3 |
| `cm05` | In trạng thái |
| `cm06` | Phát lại bài vừa phát |
| `cm07` | Chọn file khác |
| `cm08` | Pause/resume |
| `cm09` | In hướng dẫn |
| `scan`, `devices`, `list`, `play 1`, `pause`, `resume`, `stop` | Lệnh chữ tương ứng |
| `ping` | In trạng thái kết nối TCP |

Số menu `0..9` chỉ là nhãn hiển thị UART, không phải lệnh. Hãy gửi `cm00` đến
`cm09` hoặc lệnh chữ. Lệnh `channel` không còn được dùng.

Sau khi scan, danh sách thiết bị Bluetooth được lưu trong NVS WROOM. `cm02` có
thể dùng danh sách đó sau khi khởi động lại, không cần scan lại nếu cache còn
hợp lệ.
Trên UART S3, sau khi kết nối loa, nhập `1` để chọn file WROOM SPIFFS hoặc `2`
để stream PCM từ MP3 trong SPIFFS S3. Danh sách chỉ được lấy sau khi chọn
nguồn. Với file WROOM, A2DP giữ pause trong lúc hiện danh sách và chỉ phát sau
khi chọn bài. Với file S3, S3 giải mã/resample sang PCM stereo 44.1 kHz; WROOM
đưa PCM nhận qua TCP vào ring buffer mà callback A2DP đang sử dụng. File nguồn
S3 được đóng gói từ thư mục `D:\git\AI_BOT\data` thành SPIFFS riêng và mount
tại `/music`; assets của firmware không bị thay thế.

Trên UART WROOM, `cm02` chỉ phát file local trên WROOM (nguồn 1). Để dùng
nguồn 2/stream, hãy bắt đầu `cm02` trên UART S3; UART WROOM không khởi tạo luồng
chọn file SPIFFS hoặc stream từ S3.

## Giao thức TCP

- S3 là server TCP tại `192.168.4.1:3333`; WROOM là client.
- Lệnh, trạng thái và PCM đều dùng bản ghi `bt_link_packet_t` cố định (protocol
  v2); code xử lý partial send/receive vì TCP không bảo toàn ranh giới gói.
- WROOM gửi `link:connected` khi kết nối và heartbeat mỗi 5 giây. S3 dùng gói
  này để xác nhận WROOM đã sẵn sàng rồi gửi `cm00`.
- Các dòng trạng thái được gửi theo thứ tự trên TCP; lệnh được retry với cùng
  sequence nếu S3 chưa nhận phản hồi.
- Trong lúc scan hoặc thao tác Bluetooth dài, TCP vẫn giữ kết nối và status
  được chuyển tiếp về UART S3.
- Khi phát file S3, gói PCM được chuyển trực tiếp vào ring buffer A2DP; nếu ring
  đầy, TCP nhận backpressure để tránh làm tràn bộ đệm.

## Kiểm tra và xử lý sự cố

1. Nạp firmware mới cho cả hai board.
2. S3 cần báo `SoftAP AI_BOT san sang` và `TCP server san sang`.
3. WROOM cần báo đã nhận IP, sau đó `TCP link toi S3 da ket noi`.
4. UART S3 cần hiện `[WROOM] ket noi TCP qua Wi-Fi AP AI_BOT`; tiếp theo menu từ
   `cm00` sẽ được chuyển tới UART S3.
5. Nếu TCP mất, kiểm tra SSID/mật khẩu trên hai header, nguồn của cả hai board,
   log Wi-Fi reconnect và địa chỉ server `192.168.4.1:3333`.

Wi-Fi và Bluetooth Classic cùng chạy trên WROOM; thay đổi Wi-Fi/AP hoặc reconnect
có thể làm A2DP bị ảnh hưởng ngắn. Nếu Bluetooth không hoạt động, dùng UART
WROOM kiểm tra riêng A2DP trước, rồi mới kiểm tra log TCP.

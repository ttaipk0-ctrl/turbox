# TURBOX CLUSTER - ĐẶC TẢ KIẾN TRÚC & QUY TRÌNH HỆ THỐNG (PROJECT FLOW)
> TÀI LIỆU NÀY LÀ MỎ NEO NGỮ CẢNH (SINGLE SOURCE OF TRUTH) CỦA DỰ ÁN.
> Mọi cập nhật code, tính năng mới hoặc chỉnh sửa trong tương lai PHẢI TUÂN THỦ 100% các nguyên tắc bên dưới.

---

## I. NGUYÊN TẮC BẤT BIẾN (IMMUTABLE RULES - KHÔNG ĐƯỢC PHÁ VỠ)

1. deploy.sh CHỈ LÀ TRÌNH TÌM VÀ CHẠY BINARY (Silent Production Runner)
   - Nhiệm vụ duy nhất: Nhận diện OS (darwin/linux) & Kiến trúc (x86_64/arm64) -> Chạy file binary tương ứng trong bin/ bằng nohup.
   - Thiết kế tối giản, sạch sẽ (stealth), không in log lộ URL hay giải thích dài dòng để gửi client an toàn.
   - Tuyệt đối KHÔNG viết logic kinh doanh, tải engine, nạp token hay lệnh Docker vào deploy.sh.

2. ZERO-DOCKER TRÊN MÁY CLIENT / WORKER
   - Nhiều thiết bị và VPS không cài được Docker. Hệ thống PHẢI chạy hoàn toàn Native 100%.
   - Không được yêu cầu cài Docker hay hiển thị hướng dẫn Docker gây rối.

3. THƯ MỤC bin/ CHỈ CHỨA DUY NHẤT 4 FILE BINARY (Fat Binary Architecture)
   bin/
   ├── agent_darwin_x86_64     # Binary macOS Intel (đã merge sẵn payload engine)
   ├── agent_darwin_arm64      # Binary macOS Apple Silicon (đã merge sẵn payload engine)
   ├── agent_linux_x86_64      # Binary Linux x86 (đã merge sẵn payload engine)
   └── agent_linux_arm64       # Binary Linux ARM (đã merge sẵn payload engine)
   - Tuyệt đối KHÔNG sinh thêm file thực thi phụ hay thư mục con trong bin/.
   - Engine kiếm tiền được GitHub Actions đóng gói và merge trực tiếp vào đuôi 4 file binary này.

4. agent.cpp LÀ TRUNG TÂM ĐIỀU PHỐI ĐỘC LẬP TRÊN WORKER (Thuật toán tinh gọn)
   - Tự đọc chính nó để trích xuất payload engine ra thư mục đệm /tmp/.
   - Nạp Token tự động và chạy engine native (Zero-Docker).
   - Tự động bắt log thực tế từ engine (/tmp/.tb_tm.log) để gửi về Server giúp theo dõi kết nối & lưu lượng.
   - Gửi Telemetry phần cứng kèm Log Engine định kỳ 15 giây về cluster.php.
   - Tự hồi sinh (auto-revive) engine nếu bị crash hoặc tắt.

5. cluster.php LÀ MASTER SERVER & DASHBOARD ĐIỀU HÀNH
   - Lưu trữ trạng thái worker (cluster_state.json), nhật ký (cluster_logs.txt).
   - Tiếp nhận và hiển thị log engine trực tiếp (Live Output) trên từng worker node.
   - Quản lý tập trung Token các mạng kiếm tiền, tính toán tài chính, dự phóng và ETA rút tiền.
   - Web UI thời gian thực, không cần database bên ngoài (flat-file an toàn và siêu tốc).

---

## II. BẢN ĐỒ CHỨC NĂNG CỦA TỪNG FILE
- PROJECT_FLOW.md: Bản đặc tả kiến trúc làm mỏ neo ngữ cảnh chống code sai lệch.
- deploy.sh: Launcher siêu nhẹ (tìm binary và chạy).
- agent.cpp: Mã nguồn C++ Worker (tự bung payload, chạy engine, telemetry).
- .github/workflows/build.yml: GitHub Actions build cross-platform & merge engine vào binary.
- cluster.php: Master server tiếp nhận heartbeat & dashboard điều hành.
- bin/agent_*: 4 binary độc lập duy nhất của toàn hệ thống.

---

## III. QUY TRÌNH VẬN HÀNH (WORKFLOW)
1. GitHub Actions Build: Biên dịch agent.cpp -> Tải engine -> Merge payload -> Đẩy 4 file vào bin/.
2. Triển khai máy con: git pull && ./deploy.sh
3. Binary khởi chạy: Tự bung engine ra /tmp/ -> Nạp token -> Chạy native -> Gửi Telemetry lên cluster.php.
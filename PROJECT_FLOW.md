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
   - Hệ thống tập trung vào 4 Engine chuẩn đã implement native:
     + **TraffMonetizer** (`TraffMonetizer.cpp`): Tối ưu chia sẻ băng thông đa luồng, hỗ trợ cả Datacenter và Residential IP.
     + **EarnFM** (`EarnFM.cpp`): Tối ưu tuyệt đối 100% cho IP VPS / Datacenter.
     + **Honeygain** (`Honeygain.cpp`): Dành cho máy IP dân cư (Residential IP / Home / Office).
     + **Pawns** (`Pawns.cpp`): Dành cho máy IP dân cư (Residential IP / Home / Office).
   - Tách biệt Log cho từng Process:
     + TraffMonetizer ghi ra `/tmp/.tb_tm.log`
     + EarnFM ghi ra `/tmp/.tb_efm.log`
     + Honeygain ghi ra `/tmp/.tb_hg.log`
     + Pawns ghi ra `/tmp/.tb_pawns.log`
   - Bắt kết nối Socket thực tế & Parse 10 dòng log mới nhất:
     + Kiểm tra trạng thái kết nối TCP ESTABLISHED qua `/proc/net/tcp` (Linux) hoặc `lsof` (macOS).
     + Đọc và parse log mới nhất, tạo báo cáo chuẩn hóa gửi định kỳ 15 giây về turbox_server.php.
   - Tự hồi sinh (auto-revive) engine nếu bị crash hoặc tắt.

5. turbox_server.php LÀ MASTER SERVER & DASHBOARD ĐIỀU HÀNH
   - Lưu trữ trạng thái worker và dữ liệu trong SQLite: file database `cluster.db` nằm trong thư mục `turbox/` cùng cấp với file PHP (tự động tạo nếu chưa có).
   - Tiếp nhận và hiển thị log engine trực tiếp (Live Output) trên từng worker node.
   - Quản lý tập trung Token các mạng kiếm tiền, tính toán tài chính, dự phóng và ETA rút tiền.
   - Web UI thời gian thực, không cần database bên ngoài (SQLite độc lập an toàn và siêu tốc).
     + Tự động dọn dẹp (Auto-Prune): Xóa nhật ký log cũ hơn 24 giờ và tự động xóa vĩnh viễn các worker node offline quá 7 ngày để giữ database luôn nhẹ, sạch và tối ưu.

---

## II. BẢN ĐỒ CHỨC NĂNG CỦA TỪNG FILE
- PROJECT_FLOW.md: Bản đặc tả kiến trúc làm mỏ neo ngữ cảnh chống code sai lệch.
- deploy.sh: Launcher siêu nhẹ (tìm binary và chạy).
- agent.cpp: Mã nguồn C++ Worker (tự bung payload, chạy engine, telemetry).
- TraffMonetizer.cpp: Engine TraffMonetizer native.
- EarnFM.cpp: Engine EarnFM native cho Datacenter VPS.
- Honeygain.cpp: Engine Honeygain native cho IP dân cư.
- Pawns.cpp: Engine Pawns.app native cho IP dân cư.
- .github/workflows/build.yml: GitHub Actions build cross-platform & merge engine vào binary.
- turbox_server.php: Master server tiếp nhận heartbeat & dashboard điều hành.
- bin/agent_*: 4 binary độc lập duy nhất của toàn hệ thống.

---

## III. QUY TRÌNH VẬN HÀNH (WORKFLOW)
1. GitHub Actions Build: Biên dịch agent.cpp -> Tải engine -> Merge payload -> Đẩy 4 file vào bin/.
2. Triển khai máy con: git pull && ./deploy.sh
3. Binary khởi chạy: Tự bung engine ra /tmp/ -> Nạp token -> Chạy native -> Gửi Telemetry lên turbox_server.php.

---

## IV. DANH SÁCH ENGINE BỊ LOẠI BỎ & GHI CHÚ KỸ THUẬT (REMOVED SERVICES & POST-MORTEM)
1. **Kryptex (`Kryptex.cpp` - ĐÃ LOẠI BỎ HOÀN TOÀN):**
   - **Lý do:** Google Colab, GCP và các nhà cung cấp Cloud VPS có cơ chế Virtual Machine Threat Detection nghiêm ngặt cấm đào coin (Cryptomining). Việc chạy miner sẽ bị hệ thống quét bộ nhớ/mạng và dẫn đến ban nick, khóa tài khoản vĩnh viễn. Để đảm bảo an toàn tuyệt đối cho người dùng, Kryptex đã bị loại bỏ hoàn toàn khỏi kiến trúc dự án.
2. **Bitping (`Bitping.cpp` - ĐÃ LOẠI BỎ HOÀN TOÀN):**
   - **Lý do:** Bitping có cơ chế kiểm tra năng lực (Capacity Check) khắt khe đòi hỏi quyền mạng cấp thấp (Raw ICMP Socket) và các ràng buộc tài nguyên mạng không phù hợp với môi trường VPS/container chuẩn (thường gây lỗi `"some protocol failed their capacity check"`). Do đó đã được loại bỏ hoàn toàn để giữ hệ thống tinh gọn, ổn định và tối ưu hiệu suất.

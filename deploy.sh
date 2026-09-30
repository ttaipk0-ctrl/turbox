#!/bin/sh
# Turbox Cluster - Silent Production Runner (Compliant with PROJECT_FLOW.md)
cd "$(dirname "$0")" || exit 1

OS=$(uname -s | tr '[:upper:]' '[:lower:]')
case "$(uname -m)" in
    x86_64|amd64) A="x86_64" ;;
    aarch64|arm64) A="arm64" ;;
    *) A="x86_64" ;;
esac

BIN="./bin/agent_${OS}_${A}"
PID_FILE=".agent.pid"
LOG_FILE=".agent.log"

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
        exec sh "$0"
        ;;
    log|logs)
        [ -f "$LOG_FILE" ] && cat "$LOG_FILE" || echo "[INFO] No log found"
        exit 0
        ;;
esac

if [ ! -f "$BIN" ]; then
    echo "[ERROR] Binary not found: $BIN"
    exit 1
fi

chmod +x "$BIN" 2>/dev/null
[ "$OS" = "darwin" ] && xattr -c "$BIN" 2>/dev/null

if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE" 2>/dev/null)" 2>/dev/null; then
    echo "[OK] Already running (PID: $(cat "$PID_FILE"))"
    exit 0
fi

nohup "$BIN" "$@" > "$LOG_FILE" 2>&1 &
PID=$!
echo "$PID" > "$PID_FILE"

sleep 1
if kill -0 "$PID" 2>/dev/null; then
    echo "[OK] Started (PID: $PID)"
    exit 0
fi

echo "[ERROR] Failed to start $BIN"
rm -f "$PID_FILE"

# Tự động in chi tiết lỗi ra màn hình ngay lập tức (không bắt người dùng gõ manual)
if [ -f "$LOG_FILE" ] && [ -s "$LOG_FILE" ]; then
    cat "$LOG_FILE"
else
    # Nếu nohup chưa ghi kịp hoặc OS kill ngay khi nạp dynamic linker, chạy trực tiếp để in lỗi OS (GLIBC, format, lib...)
    "$BIN" 2>&1
fi
exit 1

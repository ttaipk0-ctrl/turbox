#!/bin/sh
cd "$(dirname "$0")" || exit 1

OS=$(uname -s | tr '[:upper:]' '[:lower:]')
case "$(uname -m)" in
    x86_64|amd64) A="x86_64" ;;
    aarch64|arm64) A="arm64" ;;
    *) A="x86_64" ;;
esac

BIN="./bin/agent_${OS}_${A}"
BIN_NAME="agent_${OS}_${A}"
PID_FILE=".agent.pid"
UPDATER_PID_FILE=".updater.pid"
LOG_FILE=".agent.log"
LOCK_FILE="/tmp/.turbox_agent.lock"

case "$1" in
    status)
        if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE" 2>/dev/null)" 2>/dev/null; then
            echo "[OK] Running (PID: $(cat "$PID_FILE"))"
            exit 0
        fi
        P=$(pgrep -x "$BIN_NAME" 2>/dev/null || pgrep -f "$BIN" 2>/dev/null)
        if [ -n "$P" ]; then
            echo "[OK] Running (PID: $(echo $P | tr '\n' ' '))"
            exit 0
        fi
        echo "[INFO] Not running"
        exit 1
        ;;
    stop)
        K=0
        if [ -x "$BIN" ]; then
            "$BIN" stop >/dev/null 2>&1 || true
        fi
        if [ -f "$PID_FILE" ]; then
            PID=$(cat "$PID_FILE" 2>/dev/null)
            if [ -n "$PID" ]; then
                kill -15 "$PID" 2>/dev/null || kill -9 "$PID" 2>/dev/null
                K=1
            fi
            rm -f "$PID_FILE"
        fi
        P=$(pgrep -x "$BIN_NAME" 2>/dev/null || pgrep -f "$BIN" 2>/dev/null)
        if [ -n "$P" ]; then
            kill -9 $P 2>/dev/null
            K=1
        fi
        rm -f "$LOCK_FILE" 2>/dev/null || true
        pkill -9 -f "/tmp/.tb_efm/earnfm" 2>/dev/null || true
        pkill -9 -f "kryptex" 2>/dev/null || true
        pkill -9 -f "tb_gpu_worker" 2>/dev/null || true
        pkill -9 -f "bitping" 2>/dev/null || true
        rm -f /tmp/.tb_kryptex.pid /tmp/.tb_gpu.pid /tmp/.tb_bp.pid 2>/dev/null || true
        if [ -f "$UPDATER_PID_FILE" ]; then
            kill -9 "$(cat "$UPDATER_PID_FILE" 2>/dev/null)" 2>/dev/null || true
            rm -f "$UPDATER_PID_FILE"
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

# Kiem tra tien trinh agent dang chay san trong he thong de tranh xung dot va loi fail to start
EXISTING_P=$(pgrep -x "$BIN_NAME" 2>/dev/null || pgrep -f "$BIN" 2>/dev/null)
if [ -n "$EXISTING_P" ]; then
    echo "$EXISTING_P" > "$PID_FILE"
    echo "[OK] Already running (PID: $(echo $EXISTING_P | tr '\n' ' '))"
    exit 0
fi

nohup "$BIN" "$@" > "$LOG_FILE" 2>&1 &
PID=$!
echo "$PID" > "$PID_FILE"
sleep 1

if kill -0 "$PID" 2>/dev/null; then
    # Khoi chay auto git pull worker moi 1h (try-catch, fail thi skip de khong anh huong agent)
    if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        (
            while true; do
                sleep 3600
                git pull --rebase --autostash >/dev/null 2>&1 || git pull --ff-only >/dev/null 2>&1 || git pull >/dev/null 2>&1 || true
            done
        ) >/dev/null 2>&1 &
        echo $! > "$UPDATER_PID_FILE"
    fi
    echo "[OK] Started (PID: $PID)"
    exit 0
fi

echo "[ERROR] Failed to start $BIN"
rm -f "$PID_FILE"
if [ -f "$LOG_FILE" ] && [ -s "$LOG_FILE" ]; then
    cat "$LOG_FILE"
else
    "$BIN" 2>&1
fi
exit 1

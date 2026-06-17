#!/bin/sh
set -eu

HOST="${1:-127.0.0.1}"
PORT="${2:-4450}"
MOUNT_DIR="${TMPDIR:-/tmp}/smb-ipc-$$"
PAYLOAD="ping"

cleanup() {
  if mountpoint -q "$MOUNT_DIR" 2>/dev/null; then
    sudo umount "$MOUNT_DIR"
  fi

  rmdir "$MOUNT_DIR" 2>/dev/null || true
}

trap cleanup EXIT INT TERM

mkdir -p "$MOUNT_DIR"

sudo mount -t cifs "//${HOST}/IPC$" "$MOUNT_DIR" \
  -o "guest,port=${PORT},vers=3.0,sec=ntlmssp,noperm"

exec 3<>"$MOUNT_DIR/pipe/echo"
printf "%s" "$PAYLOAD" >&3

REPLY="$(dd bs=1 count=${#PAYLOAD} <&3 2>/dev/null)"

if [ "$REPLY" != "$PAYLOAD" ]; then
  printf 'echo mismatch: expected "%s", got "%s"\n' "$PAYLOAD" "$REPLY" >&2
  exit 1
fi

printf 'echo ok: %s\n' "$REPLY"

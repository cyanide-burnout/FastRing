#!/bin/sh
set -eu

HOST="${1:-127.0.0.1}"
PORT="${2:-4450}"
PAYLOAD="${3:-ping}"
TMP_DIR="${TMPDIR:-/tmp}/sombrero-pipe-$$"
SOURCE="$TMP_DIR/pipe_echo_client.c"
BINARY="$TMP_DIR/pipe_echo_client"

cleanup() {
  rm -rf "$TMP_DIR"
}

trap cleanup EXIT INT TERM
mkdir -p "$TMP_DIR"

cat >"$SOURCE" <<'EOF'
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#ifndef SMB2_SEC_NTLMSSP
#define SMB2_SEC_NTLMSSP  1
#endif

static int TryPath(struct smb2_context* smb2, const char* path, const uint8_t* payload, size_t length)
{
  struct smb2fh* handle;
  uint8_t* reply;
  int written;
  int read_count;

  handle = smb2_open(smb2, path, O_RDWR);

  if (handle == NULL)
  {
    fprintf(stderr, "open(%s) failed: %s\n", path, smb2_get_error(smb2));
    return -1;
  }

  written = smb2_write(smb2, handle, payload, (uint32_t)length);

  if (written < 0)
  {
    fprintf(stderr, "write(%s) failed: %s\n", path, smb2_get_error(smb2));
    smb2_close(smb2, handle);
    return 1;
  }

  if ((size_t)written != length)
  {
    fprintf(stderr, "short write(%s): %d of %zu\n", path, written, length);
    smb2_close(smb2, handle);
    return 1;
  }

  reply = (uint8_t*)calloc(1, length + 1U);

  if (reply == NULL)
  {
    fprintf(stderr, "calloc failed\n");
    smb2_close(smb2, handle);
    return 1;
  }

  read_count = smb2_read(smb2, handle, reply, (uint32_t)length);

  if (read_count < 0)
  {
    fprintf(stderr, "read(%s) failed: %s\n", path, smb2_get_error(smb2));
    free(reply);
    smb2_close(smb2, handle);
    return 1;
  }

  if (((size_t)read_count != length) ||
      (memcmp(reply, payload, length) != 0))
  {
    fprintf(stderr, "echo mismatch on %s: expected \"%s\", got \"%.*s\"\n",
            path,
            payload,
            read_count,
            reply);
    free(reply);
    smb2_close(smb2, handle);
    return 1;
  }

  printf("echo ok via %s: %.*s\n", path, read_count, reply);
  free(reply);

  if (smb2_close(smb2, handle) < 0)
  {
    fprintf(stderr, "close(%s) failed: %s\n", path, smb2_get_error(smb2));
    return 1;
  }

  return 0;
}

int main(int argc, char** argv)
{
  static const char* candidates[] =
  {
    "\\\\pipe\\\\echo",
    "pipe/echo",
    "echo"
  };
  struct smb2_context* smb2;
  const char* host;
  const char* port;
  char server[512];
  const char* payload;
  size_t index;
  int result;

  if (argc != 4)
  {
    fprintf(stderr, "usage: %s <host> <port> <payload>\n", argv[0]);
    return 2;
  }

  host    = argv[1];
  port    = argv[2];
  payload = argv[3];
  smb2    = smb2_init_context();

  if (smb2 == NULL)
  {
    fprintf(stderr, "smb2_init_context() failed\n");
    return 1;
  }

  if (snprintf(server, sizeof(server), "%s:%s", host, port) >= (int)sizeof(server))
  {
    fprintf(stderr, "server string too long\n");
    smb2_destroy_context(smb2);
    return 1;
  }

  smb2_set_authentication(smb2, SMB2_SEC_NTLMSSP);
  smb2_set_user(smb2, "GUEST");
  smb2_set_password(smb2, "");

  if (smb2_connect_share(smb2, server, "IPC$", "GUEST") < 0)
  {
    fprintf(stderr, "smb2_connect_share(%s, IPC$) failed: %s\n", server, smb2_get_error(smb2));
    smb2_destroy_context(smb2);
    return 1;
  }

  result = 1;

  for (index = 0; index < (sizeof(candidates) / sizeof(candidates[0])); index++)
  {
    result = TryPath(smb2, candidates[index], (const uint8_t*)payload, strlen(payload));

    if (result == 0)
    {
      break;
    }
  }

  if (smb2_disconnect_share(smb2) < 0)
  {
    fprintf(stderr, "smb2_disconnect_share() failed: %s\n", smb2_get_error(smb2));
    result = 1;
  }

  smb2_destroy_context(smb2);
  return result;
}
EOF

if pkg-config --exists libsmb2 2>/dev/null; then
  CFLAGS="$(pkg-config --cflags libsmb2)"
  LIBS="$(pkg-config --libs libsmb2)"
else
  CFLAGS="${LIBSMB2_CFLAGS:-}"
  LIBS="${LIBSMB2_LIBS:--lsmb2}"
fi

cc -O2 $CFLAGS "$SOURCE" $LIBS -o "$BINARY"
"$BINARY" "$HOST" "$PORT" "$PAYLOAD"

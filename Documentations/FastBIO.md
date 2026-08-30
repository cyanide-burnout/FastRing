# FastBIO API Reference

Header: `Ring/FastBIO.h`

`FastBIO` is an OpenSSL `BIO` whose transport is FastRing/io_uring. TLS state stays
entirely inside libssl — `FastBIO` only moves bytes, and implements the BIO control
hooks libssl needs to negotiate kernel TLS offload.

It is normally used through `SSLSocket` (`Documentations/SSLSocket.md`), which owns the
`SSL*` and the session state machine. Use `FastBIO` directly only when driving OpenSSL
yourself.

## API

```c
BIO* CreateFastBIO(
  struct FastRing* ring,
  struct FastRingBufferProvider* provider,
  struct FastBufferPool* inbound,
  struct FastBufferPool* outbound,
  int handle,
  uint64_t options,
  uint32_t granularity,
  uint32_t limit,
  HandleFastBIOEvent function,
  void* closure);
```

The returned `BIO*` is handed to `SSL_set_bio()`. It is destroyed by `BIO_free()`, or
implicitly by `SSL_free()`, and teardown is deferred until outstanding ring operations
complete.

Parameters:

- `options` is an OpenSSL `SSL_OP_*` mask. Only `SSL_OP_ENABLE_KTLS` is interpreted:
  without it `FASTBIO_FLAG_KTLS_AVAILABLE` is cleared, so io_uring zerocopy sends stay
  usable on the socket.
- `granularity` is the **allocation quantum for outbound buffers**: every send buffer
  is rounded up to a multiple of it, which is what leaves headroom for the coalescing
  described below. A value around one TLS record is typical.
- `limit` bounds how many outbound SQEs may be in flight. It is clamped to half the
  submission queue, which is also the default for `0`. Reaching it raises back-pressure
  rather than growing the queue.
- `handle` is an already connected or accepted socket.

## Event Callback

```c
typedef void (*HandleFastBIOEvent)(struct FastBIO* engine, int event, int parameter);
```

| `event` | Meaning | `parameter` |
| --- | --- | --- |
| `POLLIN` | Ciphertext arrived and is queued | Bytes currently queued |
| `POLLOUT` | The outbound queue drained | `0` |
| `POLLERR` | Transport error | Positive errno, or `ENOMSG` for a malformed datagram |
| `POLLHUP` | Peer closed, or the engine is being torn down | Positive errno, or `0` |
| `0` | Progress poke requested through `FASTBIO_CTRL_TOUCH` | `0` |

The handler runs in the ring thread. Its job is to give OpenSSL a chance to make
progress — call `SSL_read()` / `SSL_write()` / the handshake, as `SSLSocket` does.

## Two Receive Paths

This is the part that is not visible from the API, and it matters when reading the
sources or chasing a stall.

- **Asynchronous receive** (the steady state). A multishot `recvmsg` fills buffers
  from the provider; completions are appended to an inbound queue, and `BIO_read()`
  copies out of that queue. No syscall happens on the read path at all.
- **Synchronous receive**. The engine polls the socket and `BIO_read()` performs a
  plain `recv(MSG_DONTWAIT)` directly. This is used while libssl must drive the socket
  itself, and `FASTBIO_FLAG_POLL_PROGRESS` records that such a read made progress, so
  the caller's drain loop knows to go round again.

`FASTBIO_CTRL_ENSURE` switches the engine to the asynchronous path. `SSLSocket` issues
it exactly once, when the handshake completes.

## Outbound Coalescing

`BIO_write()` does not allocate per call. When the head of the outbound queue is still
a normal send whose buffer has room, the new record is **appended into that same
buffer** and the pending length grown. Successive small TLS records therefore leave as
one `sendmsg`, which is why `granularity` is worth tuning: it decides how much slack
each buffer carries.

Coalescing is skipped when kTLS TX is active, since the kernel frames the records.

## Control Codes

Beyond the standard `BIO_CTRL_*`, the engine implements:

| Code | Effect |
| --- | --- |
| `FASTBIO_CTRL_ENSURE` (98) | Drop `FASTBIO_FLAG_KTLS_AVAILABLE` if neither direction actually negotiated kTLS, and switch to asynchronous receive |
| `FASTBIO_CTRL_TOUCH` (99) | Queue a NOP so the engine re-enters its progress loop on the next ring iteration |

`FASTBIO_CTRL_TOUCH` is how an application asks for another pass without doing I/O —
`SSLSocket` uses it after creation and after a write that left data pending. Both are
issued with `BIO_ctrl()`.

## kTLS

libssl negotiates the offload through the standard control codes, which the engine
implements:

- `BIO_CTRL_SET_KTLS` with `argument1 == 0` sets up **RX**, non-zero sets up **TX**.
- `BIO_CTRL_GET_KTLS_SEND` / `BIO_CTRL_GET_KTLS_RECV` report the current state.
- `BIO_CTRL_SET_KTLS_TX_SEND_CTRL_MSG` / `BIO_CTRL_CLEAR_KTLS_TX_CTRL_MSG` set the
  record type used for the next control record.

TX setup is submitted through io_uring as a **hard-linked pair** of
`SOCKET_URING_OP_SETSOCKOPT` commands — `TCP_ULP` then `TLS_TX` — so the two cannot be
interleaved with other traffic. RX setup uses ordinary `setsockopt()`.

Engine flags report the outcome:

- `FASTBIO_FLAG_KTLS_AVAILABLE` — the socket accepted `TCP_ULP`, offload may be used
- `FASTBIO_FLAG_KTLS_RECEIVE` — RX runs over kTLS
- `FASTBIO_FLAG_KTLS_SEND` — TX runs over kTLS
- `FASTBIO_FLAG_KTLS_ONCE` — setup has been attempted; do not retry
- `FASTBIO_FLAG_POLL_PROGRESS` — a synchronous read made progress this pass

kTLS is opportunistic throughout: an unsupported cipher, direction, kernel or socket
state simply leaves the userspace TLS path in place. RX uses `recvmsg_multishot` so
that `TLS_GET_RECORD_TYPE` control data survives, and synthesises the record headers
libssl expects.

> **A kTLS-capable socket must never have carried `SEND_ZC` / `SENDMSG_ZC` traffic.**
> A prior zerocopy send makes kTLS appear to enable while preventing traffic from
> flowing. See the kTLS section of the top-level `README.md`.

## Buffer Sizing

`FASTBIO_BUFFER_SIZE` is the minimum buffer size the provider must supply for RX. It
accounts for `struct io_uring_recvmsg_out`, the `TLS_GET_RECORD_TYPE` control message,
the TLS record header and one full plaintext record, rounded to
`__BIGGEST_ALIGNMENT__`. A smaller buffer truncates records.

## Rules

- One `FastBIO` per socket, handed to exactly one `SSL*`.
- The engine is reference counted internally; `BIO_free()` during outstanding I/O is
  safe and completes asynchronously.
- Do not read or write the socket behind the engine's back.
- All callbacks run in the ring thread and must not block.

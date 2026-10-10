# FastBIO API Reference

Header: `Ring/FastBIO.h`

`FastBIO` is an OpenSSL `BIO` whose transport is FastRing/io_uring. TLS state stays
entirely inside libssl — `FastBIO` only moves bytes, and implements the BIO control
hooks libssl needs to negotiate kernel TLS offload.

It is normally used through `SSLSocket` ([SSLSocket.md](SSLSocket.md)), which owns the
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
complete. On success the engine owns `handle` and closes it at the end of that teardown;
when `CreateFastBIO()` returns `NULL`, `handle` is untouched.

Parameters:

- `options` is an OpenSSL `SSL_OP_*` mask. Only `SSL_OP_ENABLE_KTLS` is interpreted:
  without it `FASTBIO_FLAG_KTLS_AVAILABLE` is cleared, so io_uring zerocopy sends stay
  usable on the socket.
- `granularity` is the **allocation quantum for outbound buffers**: every send buffer
  is rounded up to a multiple of it, which is what leaves headroom for the coalescing
  described below. A value around one TLS record is typical; `0` means
  `SSL3_RT_MAX_PLAIN_LENGTH` (16384).
- `limit` caps the number of descriptors queued for the next outbound batch. It is
  clamped to half the submission queue, which is also the default for `0`. See
  [Write Throttle](#write-throttle).
- `provider` supplies the receive buffers and must be built with
  `AllocateRingFastBuffer()` over a `FastBufferPool`: received buffers are converted with
  `FAST_BUFFER()`. See [Buffer Sizing](#buffer-sizing).
- `handle` is an already connected or accepted socket.

## Event Callback

```c
typedef void (*HandleFastBIOEvent)(struct FastBIO* engine, int event, int parameter);
```

| `event` | Meaning | `parameter` |
| --- | --- | --- |
| `POLLIN` | Asynchronous receive: ciphertext arrived and is queued | Bytes currently queued |
| poll events | Synchronous receive: the socket poll fired; `event` is its `revents` (`POLLIN`, `POLLERR`, `POLLHUP` or a combination) | `0` |
| `POLLOUT` | The batch in flight was accepted; `BIO_write()` is accepted again | `0` |
| `POLLERR` | A receive error the multishot survives, a malformed or empty `recvmsg` (`ENOMSG`), a send failure, or a failed kTLS TX `setsockopt` | Positive errno |
| `POLLHUP` | End of stream or a fatal receive error, a failed socket poll, or the ring is being released | Positive errno, or `0` |
| `0` | Progress poke requested through `FASTBIO_CTRL_TOUCH` | `0` |

`BIO_free()` detaches the handler first, so it produces no event. During `ReleaseFastRing()`
`POLLHUP` may arrive twice, once from the poll and once from the receive.

The handler runs in the ring thread. Its job is to give OpenSSL a chance to make
progress — call `SSL_read()` / `SSL_write()` / the handshake, as `SSLSocket` does.

## Two Receive Paths

This is the part that is not visible from the API, and it matters when reading the
sources or chasing a stall.

- **Asynchronous receive** (the steady state). A multishot `recvmsg` fills buffers
  from the provider; completions are appended to an inbound queue, and `BIO_read()`
  copies out of that queue. No syscall happens on the read path at all.
- **Synchronous receive**. The engine polls the socket and `BIO_read()` performs a
  plain `recv(MSG_DONTWAIT)` directly. `FASTBIO_FLAG_POLL_PROGRESS` records that such a
  read made progress, so the caller's drain loop knows to go round again.

The engine starts in synchronous receive only when kTLS is available for it (see
[kTLS](#ktls)): no record may be read into userspace before the RX key can be handed to the
kernel. It switches to the asynchronous path when the RX key is installed or on
`FASTBIO_CTRL_ENSURE`, which `SSLSocket` issues exactly once, when the handshake completes.
Without kTLS the engine starts asynchronous. A multishot receive that ends with
`-ENOBUFS`, `-ECANCELED`, or the first `-EKEYEXPIRED` after an RX key install, is re-armed
at once.

## Outbound Coalescing

Consecutive writes of one ring iteration share a buffer while it has room. When the newest
descriptor of the queued batch is still a normal send whose buffer has room, the new record
is **appended into that same buffer** and the pending length grown; otherwise a new buffer
and descriptor are allocated. Successive small TLS records therefore leave as one
`sendmsg`, which is why `granularity` is worth tuning: it decides how much slack each buffer
carries. Within a batch every send but the last carries `MSG_MORE`.

Coalescing and `MSG_MORE` are skipped when kTLS TX is active, since the kernel frames the
records.

Sends are `SENDMSG_ZC` with `MSG_WAITALL` and `IORING_RECVSEND_POLL_FIRST`, or plain
`SENDMSG` while `FASTBIO_FLAG_KTLS_AVAILABLE` is set.

## Write Throttle

At most one batch is in flight. The queued batch is submitted by a flusher at the end of the
iteration, and from then on `BIO_write()` asks for a retry (`BIO_set_retry_write()`,
`errno = EAGAIN`) until the batch has been accepted and `POLLOUT` is delivered; coalescing
into a batch that is still queued is the only write accepted meanwhile. The same retry is
asked for when the queued batch reaches `limit` descriptors, and with `errno = ENOMEM` when
a buffer or descriptor cannot be allocated.

A send failure or a failed kTLS TX `setsockopt` on the last descriptor of a batch is
reported as `POLLERR` and does not clear the throttle: `BIO_write()` keeps asking for a
retry from then on, and the session is over.

## Control Codes

Beyond the standard `BIO_CTRL_*`, the engine implements:

| Code | Effect |
| --- | --- |
| `FASTBIO_CTRL_ENSURE` (98) | Drop `FASTBIO_FLAG_KTLS_AVAILABLE` if neither direction actually negotiated kTLS, and switch to asynchronous receive |
| `FASTBIO_CTRL_TOUCH` (99) | Queue a NOP so the engine re-enters its progress loop on the next ring iteration; ignored while a touch is pending or the engine has no operation of its own outstanding |

`FASTBIO_CTRL_TOUCH` is how an application asks for another pass without doing I/O —
`SSLSocket` uses it after creation and after a write that left data pending. Both are
issued with `BIO_ctrl()`. `BIO_CTRL_DUP` and `BIO_CTRL_FLUSH` return `1`; other standard
controls, `BIO_CTRL_PENDING` and `BIO_CTRL_EOF` among them, return `0`.

## kTLS

libssl negotiates the offload through the standard control codes, which the engine
implements:

- `BIO_CTRL_SET_KTLS` with `argument1 == 0` sets up **RX**, non-zero sets up **TX**.
- `BIO_CTRL_GET_KTLS_SEND` / `BIO_CTRL_GET_KTLS_RECV` report the current state.
- `BIO_CTRL_SET_KTLS_TX_SEND_CTRL_MSG` / `BIO_CTRL_CLEAR_KTLS_TX_CTRL_MSG` set the
  record type used for the next control record.

TX setup is submitted through io_uring as a **hard-linked pair** of
`SOCKET_URING_OP_SETSOCKOPT` commands — `TCP_ULP` then `TLS_TX` — appended to the queued
batch, so it is ordered after the writes queued in the same iteration. It needs Linux 6.7
for `SOCKET_URING_OP_SETSOCKOPT`, and the module needs liburing 2.5 to build
(`io_uring_prep_cmd_sock()`). RX setup uses ordinary `setsockopt()`.

Engine flags report the outcome:

- `FASTBIO_FLAG_KTLS_AVAILABLE` — the kernel has the `tls` ULP, found by a one-time probe
  on a dummy socket at the first `CreateFastBIO()` with `SSL_OP_ENABLE_KTLS`, and this BIO
  was created with that option. Cleared by `FASTBIO_CTRL_ENSURE` when neither direction was
  negotiated. While it is set, sends do not use zerocopy
- `FASTBIO_FLAG_KTLS_RECEIVE` — RX runs over kTLS
- `FASTBIO_FLAG_KTLS_SEND` — TX runs over kTLS
- `FASTBIO_FLAG_KTLS_ONCE` — set when the RX key is installed: one `-EKEYEXPIRED` from the
  receive is tolerated (TLS 1.3 KeyUpdate) and the receive re-armed. In the module's static
  flags the same bit means that the process-wide probe has not run yet
- `FASTBIO_FLAG_POLL_PROGRESS` — a synchronous read made progress this pass

kTLS is opportunistic throughout: an unsupported cipher, direction, kernel or socket
state simply leaves the userspace TLS path in place. RX uses `recvmsg_multishot` so
that `TLS_GET_RECORD_TYPE` control data survives, and synthesises the record headers
libssl expects.

> **A kTLS-capable socket must never have carried `SEND_ZC` / `SENDMSG_ZC` traffic.**
> A prior zerocopy send makes kTLS appear to enable while preventing traffic from
> flowing. kTLS sendfile / `TLS_TX_ZEROCOPY_RO` is a separate path.

## Buffer Sizing

`FASTBIO_BUFFER_SIZE` is the minimum buffer size the provider must supply for RX. It
accounts for `struct io_uring_recvmsg_out`, the `TLS_GET_RECORD_TYPE` control message,
the TLS record header and one full plaintext record, rounded to
`__BIGGEST_ALIGNMENT__`. With kTLS RX a record that does not fit (`MSG_CTRUNC`, or no
`MSG_EOR`) makes `BIO_read()` fail with `EMSGSIZE`; the userspace stream path works with
smaller buffers.

## Rules

- One `FastBIO` per socket, handed to exactly one `SSL*`.
- The engine is reference counted internally; `BIO_free()` during outstanding I/O is
  safe and completes asynchronously.
- Do not read or write the socket behind the engine's back.
- All callbacks run in the ring thread and must not block, and every `BIO_*` call on the
  engine is made there too: its reference count and queues are not synchronised.

## Lifecycle

Objects on the ring, all owned by the engine behind the `BIO*`:

- a one-shot `POLL_ADD` (`POLLIN | POLLERR | POLLHUP`) for synchronous receive, re-armed
  after each event while the engine is in that mode;
- one multishot `recvmsg` with buffers from `provider`, submitted when the engine enters
  asynchronous receive and re-armed whenever the kernel ends it without a fatal error;
- one `sendmsg` or `sendmsg_zc` per outbound buffer, each holding one `FastBuffer`
  reference; for kTLS TX a hard-linked pair of `setsockopt` commands; one NOP per
  `FASTBIO_CTRL_TOUCH`;
- at most one flusher at a time, which submits the queued batch;
- at the end, one `IORING_OP_CLOSE` for `handle`.

The engine counts one reference for the `BIO*` and one for each outstanding operation, and
is freed when the count reaches zero.

`BIO_free()`, directly or through `SSL_free()`, detaches the handler and ends both receive
operations: one not yet submitted is rewritten as a NOP, one in the kernel is cancelled,
and one whose final completion is being handled is left to that handler. It then drops the
`BIO*` reference. Queued writes, a `close_notify` for example, are still submitted. When the
last reference goes, the queued inbound buffers are released, `IORING_OP_CLOSE` is submitted
for `handle` (`close()` when no descriptor can be allocated), and the engine is freed.
Nothing reports that moment; `provider` and the pools must stay valid until then, so run the
loop until the operations have completed before releasing them.

If `ReleaseFastRing()` runs while the engine is alive, its handlers are called with
`RING_REASON_RELEASED`: the poll and receive handlers deliver `POLLHUP` with `0`, outbound
handlers release their buffers, a pending touch delivers event `0`, and a queued close calls
`close()`. A `BIO_free()` after `ReleaseFastRing()` uses the freed ring; free the BIO before
the ring.

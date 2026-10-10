# CURLWSCore API Reference

Header: `Ring/CURLWSCore.h`

`CURLWSCore` is the recommended WebSocket client adapter. It is a thin layer over
`Fetch`: `struct CWSTransmission` embeds `struct FetchTransmission` as its first
member (`super`), so a WebSocket session is a Fetch transmission and is driven by the
same `Fetch` instance and the same ring.

## Layering

```text
FastRing  ->  Fetch (libcurl multi)  ->  CURLWSCore  ->  application
```

`CURLWSCore` installs its own libcurl callbacks on the easy handle
(`CURLOPT_WRITEFUNCTION`, `CURLOPT_HEADERFUNCTION`, and on libcurl 8.16+ also
`CURLOPT_READFUNCTION` with `CURLOPT_UPLOAD`), and registers the transmission with
`MakeExtendedFetchTransmission()` using `FETCH_OPTION_SET_HANDLER_DATA`. An
application must not overwrite those options, nor `CURLOPT_READDATA`,
`CURLOPT_WRITEDATA`, `CURLOPT_HEADERDATA` and the other options `Fetch` reserves (see
[Fetch.md](Fetch.md)); everything else on the easy handle is free (see the URL, timeout
and TLS setup in the example below).

## Lifecycle

```c
struct CWSTransmission* MakeSimpleCWSTransmission(
  struct Fetch* fetch,
  const char* location,
  struct curl_slist* headers,
  const char* token,
  HandleCWSEventFunction function,
  void* closure);

struct CWSTransmission* MakeExtendedCWSTransmission(
  struct Fetch* fetch,
  CURL* easy,
  HandleCWSEventFunction function,
  void* closure);

void CloseCWSTransmission(struct CWSTransmission* transmission);
```

- `MakeSimpleCWSTransmission()` builds the easy handle itself from `location`,
  optional `headers` and an optional bearer `token`.
- `MakeExtendedCWSTransmission()` takes an easy handle the caller has prepared. It
  takes ownership: on failure the handle is passed to `curl_easy_cleanup()`.
- `CloseCWSTransmission()` cancels the underlying Fetch transmission. Called outside a
  handler, it completes the session before it returns: frames still queued and the final
  `CWS_REASON_CLOSED` are delivered from inside the call, and the object is freed. Called
  from a handler during deferred delivery, it is recorded and carried out after the
  current delivery pass; the remaining frames of that pass are still delivered.

The transmission pointer is valid from the return of `Make*CWSTransmission()` until
`CWS_REASON_CLOSED` returns. `CWS_REASON_CLOSED` is delivered even when the handshake never
completed. **After `CWS_REASON_CLOSED` the object is freed** — clear any stored pointer
inside that callback. From a handler, close a session only with `CloseCWSTransmission()`:
`CancelFetchTransmission()` or `ReleaseFetch()` would free it while the delivery pass is
still walking its queue.

## States

`transmission->state`:

- `CWS_STATE_CONNECTING` - the HTTP upgrade is still in flight
- `CWS_STATE_CONNECTED` - the handshake succeeded, frames are being exchanged
- `CWS_STATE_REJECTED` - the event handler returned a negative value; reception has
  been abandoned and the session is closed (cancelled, unless it was already closing)

The handshake is detected in the header callback: the end of the header block
combined with a response code of `101` or `200` moves the state to
`CWS_STATE_CONNECTED` and queues the notification that becomes
`CWS_REASON_CONNECTED`.

## Event Callback

```c
typedef int (*HandleCWSEventFunction)(
  void* closure,
  struct CWSTransmission* transmission,
  int reason,
  int parameter,
  char* data,
  size_t length);
```

| Reason | `parameter` | `data` / `length` |
| --- | --- | --- |
| `CWS_REASON_CONNECTED` | `0` | An empty string, `length` `0` |
| `CWS_REASON_RECEIVED` | libcurl frame flags (`CURLWS_TEXT`, `CURLWS_BINARY`, `CURLWS_PING`, `CURLWS_PONG`, `CURLWS_CLOSE`) | The complete, reassembled frame payload |
| `CWS_REASON_CLOSED` | Completion code, see below | libcurl error text for a `-CURLcode`, otherwise `NULL` |

Return `0` to continue. **A negative return rejects the session**: the state becomes
`CWS_STATE_REJECTED` and the transmission is cancelled. Reception stops there — no
further `CWS_REASON_CONNECTED` or `CWS_REASON_RECEIVED` is delivered — but
`CWS_REASON_CLOSED` still follows, so a rejection ends like any other close: for a
rejection during normal delivery its `parameter` is `FETCH_STATUS_CANCELLED`, for one
during the final delivery at close it is the code the session was closing with. On
`CWS_REASON_CLOSED` the return value is ignored.

`parameter` on `CWS_REASON_CLOSED` follows the `Fetch` convention for negative values —
`-CURLcode`, `FETCH_STATUS_CANCELLED` or `FETCH_STATUS_INCOMPLETE`, see
[Fetch.md](Fetch.md). A non-negative value is the HTTP status of a failed upgrade, while
the session is still connecting, and `0` once it was connected.

`data` points into a message buffer owned by the transmission and is only valid for
the duration of the call. Copy anything that must outlive it. It carries a terminating
zero byte past `length` — the spare byte of the allocation — so a text frame can be
handed straight to the string functions, as with the accumulated body of
[Fetch.md](Fetch.md).

### Delivery is deferred

Received frames are not delivered from inside the libcurl write callback. They are
queued and flushed through a FastRing flush handler, armed at most once per loop
iteration, so handlers run after CQ processing in the ring thread like any other
FastRing callback. A frame that completes during a callback is appended to the queue being
delivered and handed out after the current callback returns, never recursively. Frames
still queued when the session ends are delivered in place, right before
`CWS_REASON_CLOSED`, from inside whatever ends it: `CloseCWSTransmission()`,
`ReleaseFetch()` or the completion path of `Fetch`.

### Frame reassembly

A frame that libcurl delivers across several write callbacks is reassembled internally
using `curl_ws_meta()` `offset` / `bytesleft`, so the handler sees one whole frame.
Two reassembly slots are kept in parallel — one for data frames, one for control
frames (`PING` / `PONG` / `CLOSE`) — so a control frame interleaved into a partially
delivered data frame does not corrupt it. A frame arriving out of order relative to
its offset aborts the transfer.

**Continuation frames are not merged.** Reassembly works at frame level, and the slot
is chosen by control-versus-data only, so a message fragmented at protocol level
arrives as several `CWS_REASON_RECEIVED` events, each with the flags libcurl reports for
it in `parameter` (`CURLWS_CONT` marks fragmentation; its exact use differs between
libcurl versions). An application that has to deal with a peer fragmenting its messages
joins them itself.

## Sending

```c
struct CWSMessage* AllocateCWSMessage(struct CWSTransmission* transmission, size_t length, int type);
void TransmitCWSMessage(struct CWSMessage* message);
```

The sequence is always allocate, fill, **set the real length**, transmit:

```c
struct CWSMessage* message;

if (message = AllocateCWSMessage(transmission, 64, CURLWS_TEXT))
{
  message->length = sprintf(message->buffer, "42[\"join\", \"everything\"]");
  TransmitCWSMessage(message);
}
```

`AllocateCWSMessage()` presets `message->length` to the requested `length`, and that
value — not the amount actually written — is what goes on the wire. Allocating a
generous buffer and then assigning the real size to `message->length` is the normal
pattern. When the payload size is known exactly, allocate exactly and leave `length`
alone:

```c
if (message = AllocateCWSMessage(transmission, sizeof(struct PeakSetupData), CURLWS_BINARY))
{
  struct PeakSetupData* payload = (struct PeakSetupData*)message->buffer;
  ...
  TransmitCWSMessage(message);
}
```

### Message fields

| Field | Role |
| --- | --- |
| `buffer` | Payload storage, `length` bytes plus one spare byte |
| `data` | Send cursor, preset to `buffer` — usable as the fill pointer; also the close marker when set to `NULL` |
| `length` | Number of bytes to send; **assign it if you wrote fewer than allocated** |
| `type` | libcurl frame type: `CURLWS_TEXT`, `CURLWS_BINARY`, `CURLWS_PING`, `CURLWS_PONG`, `CURLWS_CONT` |
| `size` | Allocation size, managed by the module |

Never **assign** to `data` other than the close marker below, and leave `size` alone
entirely — both belong to the module once the message is queued, `data` being the cursor
it advances across partial writes.

Reading `data` before transmitting is a different matter: until then it equals `buffer`,
so filling a message through it is equivalent, and much of the code written against this
module does exactly that:

```c
struct PeakSetupData* payload = (struct PeakSetupData*)message->data;
```

### What happens after TransmitCWSMessage()

`TransmitCWSMessage()` takes ownership and appends the message to the outbound queue.
**Do not touch the message afterwards** — once written it is moved to a per-transmission
free list and handed back by a later `AllocateCWSMessage()`.

Sending is asynchronous and driven by the queue:

- Messages leave in submission order, one WebSocket frame per message. The frame
  header is emitted once per message, with the declared `length`, so the payload size
  must be final before transmit.
- A message larger than what the transport accepts in one go is written across several
  passes; the module advances `data` and decrements `length` itself and only then
  moves on to the next message.
- The queue is drained whenever the transport is writable. On libcurl 8.16+ the read
  callback parks itself with `CURL_READFUNC_PAUSE` when the queue runs dry, and
  `TransmitCWSMessage()` un-pauses it — but **only when it enqueues into an empty
  queue**, since a non-empty queue means the sender is already running.

### No completion signal, no back-pressure

There is no event that reports "this message has been sent" — `CWS_REASON_*` covers
the receive side and shutdown only. The outbound queue is also unbounded: every
`AllocateCWSMessage()` that fails returns `NULL`, but a slow peer does not otherwise
throttle a producer. An application that can outrun its link has to do its own
accounting, for example by tracking application-level acknowledgements.

### Closing from the send path

Setting `message->data = NULL` before `TransmitCWSMessage()` makes the queued entry a
close marker: when the sender reaches it, the transmission ends — on libcurl 8.16+ through
the read callback, so `CWS_REASON_CLOSED` carries `-CURLE_ABORTED_BY_CALLBACK`, on older
versions by a cancellation with `FETCH_STATUS_CANCELLED`. This closes
the session *after* everything queued before it has been written, which
`CloseCWSTransmission()` does not do — it cancels immediately and discards whatever is
still queued.

## libcurl Version Split

The send path has two implementations, selected at compile time:

- **libcurl >= 8.16.0** - `CURLOPT_UPLOAD` plus a read callback. The queue is drained
  by libcurl; `TransmitCWSMessage()` un-pauses the handle with
  `curl_easy_pause(CURLPAUSE_SEND_CONT)` and touches the Fetch transmission.
- **older libcurl** - the module polls the active socket for `POLLOUT` through its own
  FastRing descriptor and pushes frames with `curl_ws_send()`.

The API is identical either way; the difference matters only when reading the sources
or debugging send stalls. With older libcurl, transmit only after `CWS_REASON_CONNECTED`:
the `POLLOUT` descriptor does not send before the handshake, and it is re-armed on every
loop iteration until then. A send error drops the message without an event.

## Ring Lifecycle

A session is registered with `Fetch` as a transmission, and `Fetch` frees it after the
closing notification. On the ring it uses at most one flush handler for deferred delivery,
and with libcurl older than 8.16 one `POLLOUT` descriptor on the active socket, armed by
`TransmitCWSMessage()`, resubmitted while messages remain and released by its own completion
once the queue is drained; on 8.16 and later sending uses the socket polls of `Fetch`.

When the session ends, frames still queued are delivered (unless it was rejected),
`CWS_REASON_CLOSED` is delivered, the queued, partial and free-list messages are freed, the
`POLLOUT` descriptor is discarded and the flush handler removed, and `Fetch` destroys the easy
handle and frees the session.

Everything runs on the ring thread. Release every `Fetch` before `ReleaseFastRing()`: if the
ring is released first, the flush handler and the `POLLOUT` descriptor receive
`RING_REASON_RELEASED` and do nothing, `CWS_REASON_CLOSED` is not delivered, and the sessions
can no longer be released.

## Example

`Examples/CURLWS` is the minimal client. A production pattern, including per-socket
tuning right after the handshake, looks like this:

```c
static int HandleSocketEvent(void* closure, struct CWSTransmission* transmission,
                             int reason, int parameter, char* data, size_t length)
{
  struct Transport* transport = (struct Transport*)closure;

  switch (reason)
  {
    case CWS_REASON_CONNECTED:
      transport->transmission = transmission;
      SetTransmissionServiceType(transmission);   // setsockopt() on CURLINFO_ACTIVESOCKET
      TransmitInitialData(transport);
      break;

    case CWS_REASON_RECEIVED:
      if (parameter & CURLWS_BINARY)  HandleSocketData(transport, data, length);
      if (parameter & CURLWS_PONG)    HandleSocketPong(transport, data, length);
      break;

    case CWS_REASON_CLOSED:
      transport->transmission = NULL;             // the object is gone after this call
      HandleFailureState(transport);
      break;
  }

  return 0;
}
```

The socket itself is reachable through `CURLINFO_ACTIVESOCKET` on
`transmission->super.easy`, which is how `TCP_QUICKACK` or DSCP marking is applied once
the connection is up.

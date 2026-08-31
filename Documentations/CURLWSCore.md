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
application must not overwrite those options; everything else on the easy handle is
free (see the URL, timeout and TLS setup in the example below).

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
- `CloseCWSTransmission()` cancels the underlying Fetch transmission. It does not free
  anything directly — teardown happens in the Fetch completion path, which also
  delivers the final `CWS_REASON_CLOSED`.

The transmission pointer is valid from the moment `CWS_REASON_CONNECTED` is delivered
until `CWS_REASON_CLOSED` returns. **After `CWS_REASON_CLOSED` the object is freed** —
clear any stored pointer inside that callback.

## States

`transmission->state`:

- `CWS_STATE_CONNECTING` - the HTTP upgrade is still in flight
- `CWS_STATE_CONNECTED` - the handshake succeeded, frames are being exchanged
- `CWS_STATE_REJECTED` - the event handler returned a negative value; reception has
  been abandoned and the transmission is being cancelled

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
| `CWS_REASON_CONNECTED` | `0` | Empty |
| `CWS_REASON_RECEIVED` | libcurl frame flags (`CURLWS_TEXT`, `CURLWS_BINARY`, `CURLWS_PING`, `CURLWS_PONG`, `CURLWS_CLOSE`) | The complete, reassembled frame payload |
| `CWS_REASON_CLOSED` | Completion code from `Fetch` | Error text when the transport failed, otherwise empty |

Return `0` to continue. **A negative return rejects the session**: the state becomes
`CWS_STATE_REJECTED` and the transmission is cancelled. On `CWS_REASON_CLOSED` the
return value is ignored.

`parameter` on `CWS_REASON_CLOSED` follows the `Fetch` convention exactly — a
non-negative value is the HTTP response code, a negative one is either `-CURLcode` or
`FETCH_STATUS_CANCELLED` / `FETCH_STATUS_INCOMPLETE`. See [Fetch.md](Fetch.md).

`data` points into a message buffer owned by the transmission and is only valid for
the duration of the call. Copy anything that must outlive it.

### Delivery is deferred

Received frames are not delivered from inside the libcurl write callback. They are
queued and flushed through a FastRing flush handler, armed at most once per loop
iteration, so handlers run after CQ processing in the ring thread like any other
FastRing callback. A frame that completes during a callback is therefore delivered on
the next pass, not recursively.

### Frame reassembly

Fragmented frames are reassembled internally using libcurl's `curl_ws_meta()`
`offset` / `bytesleft`, so the handler always sees whole messages. Two reassembly
slots are kept in parallel — one for data frames, one for control frames
(`PING` / `PONG` / `CLOSE`) — so a control frame interleaved into a fragmented data
message does not corrupt it. A frame arriving out of order relative to its offset
aborts the transfer.

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
| `data` | Send cursor, preset to `buffer`; also the close marker when set to `NULL` |
| `length` | Number of bytes to send; **assign it if you wrote fewer than allocated** |
| `type` | libcurl frame type: `CURLWS_TEXT`, `CURLWS_BINARY`, `CURLWS_PING`, `CURLWS_PONG`, `CURLWS_CONT` |
| `size` | Allocation size, managed by the module |

Do not touch `data` or `size` other than for the close marker below.

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
close marker: when the sender reaches it, the transmission is cancelled. This closes
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
or debugging send stalls.

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

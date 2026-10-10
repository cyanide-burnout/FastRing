# XMPPServer API Reference

Header: `Supplimentary/XMPPServer.h`

`XMPPServer` is a minimal XMPP stream acceptor on top of FastRing/FastSocket and libxml2 SAX
parsing: it accepts plaintext `jabber:client` streams and extracts a fixed set of fields from
each stanza. There is no TLS, SASL, stream negotiation or routing; replies are formatted text
written to the socket as is, without XML escaping.

## Events

- `XMPP_EVENT_CONNECTION_ACCEPT`
- `XMPP_EVENT_CONNECTION_DESTROY`
- `XMPP_EVENT_STREAM_BEGIN`
- `XMPP_EVENT_STREAM_END`
- `XMPP_EVENT_STANZA_BEGIN`
- `XMPP_EVENT_STANZA_END`

Event data (`union XMPPEventData`):

- `XMPP_EVENT_CONNECTION_ACCEPT` - `connection` is `NULL` and `data->address` is the
  peer address; returning a negative value rejects the connection
- `XMPP_EVENT_STREAM_BEGIN`, `XMPP_EVENT_STANZA_BEGIN` / `XMPP_EVENT_STANZA_END` -
  `data->store` is an array of `XMPP_STORE_LENGTH` string pointers indexed by the
  constants below
- `XMPP_EVENT_STREAM_END`, `XMPP_EVENT_CONNECTION_DESTROY` - `data` is `NULL`

Other return values are ignored. `XMPP_EVENT_STREAM_END` does not destroy the connection;
the destruction follows when the peer closes it.

## Stanza Store

`data->store` is indexed by:

| Index | Constant |
| --- | --- |
| 0 | `XMPP_STORE_STANZA_NAME` |
| 1 | `XMPP_STORE_STANZA_ID` |
| 2 | `XMPP_STORE_STANZA_TO` |
| 3 | `XMPP_STORE_STANZA_FROM` |
| 4 | `XMPP_STORE_STANZA_TYPE` |
| 5 | `XMPP_STORE_CHILD_NAME` |
| 6 | `XMPP_STORE_CHILD_LOCATION` |
| 7 | `XMPP_STORE_CHILD_PROPERTY` |
| 8 | `XMPP_STORE_MESSAGE_TYPE` |
| 9 | `XMPP_STORE_MESSAGE_TEXT` |
| 10 | `XMPP_STORE_MESSAGE_INFO` |

`XMPP_STORE_LENGTH` (11) is the array size. Unset slots are `NULL`. The child and message
fields are available at `XMPP_EVENT_STANZA_END` only. The strings live in a per-connection
scratch pad reset at every stanza and are valid during the callback only.

## Connection State

`connection->state` is a bit mask, exposed for modules that inspect a connection
directly:

- `XMPP_STATE_DESTROY` (`1 << 0`) - set by `ReleaseXMPPServer()`; like `CLOSE`, it
  suppresses `XMPP_EVENT_CONNECTION_DESTROY`
- `XMPP_STATE_FAILURE` (`1 << 1`) - a protocol or transport failure was recorded; also set
  on every destruction from the socket callback
- `XMPP_STATE_TIMEOUT` (`1 << 2`) - no inbound data for 60 s, checked every 5 s
- `XMPP_STATE_CLOSE` (`1 << 3`) - an immediate close was requested through
  `CloseXMPPConnection()`: no `</stream:stream>` is sent, and the socket is released, which
  closes it once the sends in flight have completed. It also suppresses
  `XMPP_EVENT_CONNECTION_DESTROY`: a connection the owner closed explicitly does not report
  destruction back
- `XMPP_STATE_GUARD` (`1 << 4`) - an event handler is on the stack, so
  `CloseXMPPConnection()` is deferred until the socket callback has processed the whole
  batch of received data; the remaining events of that batch are still delivered
- `XMPP_STATE_HANDLE_NAME` (`1 << 5`), `XMPP_STATE_HANDLE_VALUE` (`1 << 6`) - SAX
  parser position within the current element

## API

```c
typedef int (*HandleXMPPEvent)(
  struct XMPPServer* server,
  struct XMPPConnection* connection,
  int event,
  union XMPPEventData* data);

struct XMPPServer* CreateXMPPServer(struct FastRing* ring, HandleXMPPEvent function, void* closure, int port);
void ReleaseXMPPServer(struct XMPPServer* server);

void CloseXMPPConnection(struct XMPPConnection* connection);  // clears the closure, no DESTROY event, deferred while a handler is running
int SendMPPConnection(struct XMPPConnection* connection, const char* format, ...);
int SendVariadicXMPPConnection(struct XMPPConnection* connection, const char* format, va_list arguments);
```

The send functions return `0` when the text is queued, `-EINVAL` for empty output or an
allocation failure, and the errors of `TransmitFastSocketData()` (`-EPIPE`, `-EAGAIN`).

## Lifecycle

`CreateXMPPServer()` creates a listening TCP socket on `[::]:port` and, on the ring, one
accept descriptor re-armed after every completion (also after an error), a repeating 5 s
timeout, a buffer provider fed from the inbound pool, and the inbound and outbound pools. Only
a failure to set up the socket returns `NULL`. Each accepted connection gets a `FastSocket` and
a libxml2 push parser; the server keeps its connections in a list.

A connection is destroyed by the socket callback on a hang-up, an error or a parse error, by
the timeout, by `CloseXMPPConnection()`, or by `ReleaseXMPPServer()`;
`XMPP_EVENT_CONNECTION_DESTROY` is delivered in the first two cases. Destruction frees the
parser and the connection and releases the `FastSocket`. A connection pointer is invalid after
its destroy event, after `CloseXMPPConnection()` returns outside a handler, and after the batch
in which it was closed from a handler.

`ReleaseXMPPServer()` destroys every connection, detaches the accept handler, removes the
timeout, releases the provider and its references to the pools, closes the listening socket
and frees the server. It must not be called from an event handler. The pending accept is not
cancelled: it holds the socket in the kernel, so the port keeps listening until one more
connection completes the request, and the descriptor of that connection is not closed.

Release the server on the ring thread and before the ring. If `ReleaseFastRing()` runs first,
the accept and the timer are dropped without callbacks, every live connection is destroyed
from its socket's release path with `XMPP_EVENT_CONNECTION_DESTROY`, and the server can no
longer be released safely.

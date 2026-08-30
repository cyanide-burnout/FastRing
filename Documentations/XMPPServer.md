# XMPPServer API Reference

Header: `Supplimentary/XMPPServer.h`

`XMPPServer` implements an XMPP server on top of FastRing/FastSocket and libxml SAX parsing.

## Events

- `XMPP_EVENT_CONNECTION_ACCEPT`
- `XMPP_EVENT_CONNECTION_DESTROY`
- `XMPP_EVENT_STREAM_BEGIN`
- `XMPP_EVENT_STREAM_END`
- `XMPP_EVENT_STANZA_BEGIN`
- `XMPP_EVENT_STANZA_END`

Event data (`union XMPPEventData`):

- `XMPP_EVENT_CONNECTION_ACCEPT` - `data->address` is the peer address
- `XMPP_EVENT_STANZA_BEGIN` / `XMPP_EVENT_STANZA_END` - `data->store` is an array of
  `XMPP_STORE_LENGTH` string pointers indexed by the constants below

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

`XMPP_STORE_LENGTH` (11) is the array size. Unset slots are `NULL`.

## Connection State

`connection->state` is a bit mask, exposed for modules that inspect a connection
directly:

- `XMPP_STATE_DESTROY` (`1 << 0`) - the connection is being torn down
- `XMPP_STATE_FAILURE` (`1 << 1`) - a protocol or transport failure was recorded
- `XMPP_STATE_TIMEOUT` (`1 << 2`) - the inactivity timeout fired
- `XMPP_STATE_CLOSE` (`1 << 3`) - a graceful close was requested through
  `CloseXMPPConnection()`. It also suppresses `XMPP_EVENT_CONNECTION_DESTROY`: a
  connection the owner closed explicitly does not report destruction back
- `XMPP_STATE_GUARD` (`1 << 4`) - an event handler is on the stack, so
  `CloseXMPPConnection()` is deferred until it returns
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


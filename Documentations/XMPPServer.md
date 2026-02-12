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

## API

```c
typedef int (*HandleXMPPEvent)(
  struct XMPPServer* server,
  struct XMPPConnection* connection,
  int event,
  union XMPPEventData* data);

struct XMPPServer* CreateXMPPServer(struct FastRing* ring, HandleXMPPEvent function, void* closure, int port);
void ReleaseXMPPServer(struct XMPPServer* server);

void CloseXMPPConnection(struct XMPPConnection* connection);
int SendMPPConnection(struct XMPPConnection* connection, const char* format, ...);
int SendVariadicXMPPConnection(struct XMPPConnection* connection, const char* format, va_list arguments);
```


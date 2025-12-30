#ifndef XMPPSERVER_H
#define XMPPSERVER_H

#include "FastRing.h"
#include "FastSocket.h"

#include <stdarg.h>
#include <libxml/parser.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define XMPP_EVENT_CONNECTION_ACCEPT   0
#define XMPP_EVENT_CONNECTION_DESTROY  1
#define XMPP_EVENT_STREAM_BEGIN        2
#define XMPP_EVENT_STREAM_END          3
#define XMPP_EVENT_STANZA_BEGIN        4
#define XMPP_EVENT_STANZA_END          5

#define XMPP_STATE_DESTROY       (1 << 0)
#define XMPP_STATE_FAILURE       (1 << 1)
#define XMPP_STATE_TIMEOUT       (1 << 2)
#define XMPP_STATE_CLOSE         (1 << 3)
#define XMPP_STATE_GUARD         (1 << 4)
#define XMPP_STATE_HANDLE_NAME   (1 << 5)
#define XMPP_STATE_HANDLE_VALUE  (1 << 6)

#define XMPP_STORE_STANZA_NAME     0
#define XMPP_STORE_STANZA_ID       1
#define XMPP_STORE_STANZA_TO       2
#define XMPP_STORE_STANZA_FROM     3
#define XMPP_STORE_STANZA_TYPE     4
#define XMPP_STORE_CHILD_NAME      5
#define XMPP_STORE_CHILD_LOCATION  6
#define XMPP_STORE_CHILD_PROPERTY  7
#define XMPP_STORE_MESSAGE_TYPE    8
#define XMPP_STORE_MESSAGE_TEXT    9
#define XMPP_STORE_MESSAGE_INFO    10
#define XMPP_STORE_LENGTH          11

union XMPPEventData
{
  struct sockaddr* address;
  const char** store;
};

struct XMPPServer;
struct XMPPConnection;

typedef int (*HandleXMPPEvent)(struct XMPPServer* server, struct XMPPConnection* connection, int event, union XMPPEventData* data);

struct XMPPScratchPad
{
  char* data;
  size_t size;
  size_t length;
};

struct XMPPConnection
{
  void* closure;
  uint32_t state;
  struct timespec time;
  struct timespec outcome;
  struct XMPPServer* server;
  struct XMPPConnection* next;
  struct XMPPConnection* previous;

  int depth;
  xmlParserCtxtPtr parser;
  struct XMPPScratchPad scratch;
  const char* store[XMPP_STORE_LENGTH];

  struct FastSocket* socket;
  struct sockaddr_storage address;
};

struct XMPPServer
{
  int handle;
  void* closure;
  HandleXMPPEvent function;
  struct XMPPConnection* connections;

  struct FastRing* ring;
  struct FastBufferPool* inbound;
  struct FastBufferPool* outbound;
  struct FastRingBufferProvider* provider;

  struct FastRingDescriptor* listner;
  struct FastRingDescriptor* timeout;

  xmlSAXHandler handler;
};

struct XMPPServer* CreateXMPPServer(struct FastRing* ring, HandleXMPPEvent function, void* closure, int port);
void ReleaseXMPPServer(struct XMPPServer* server);

void CloseXMPPConnection(struct XMPPConnection* connection);
int SendMPPConnection(struct XMPPConnection* connection, const char* format, ...);
int SendVariadicXMPPConnection(struct XMPPConnection* connection, const char* format, va_list arguments);

#ifdef __cplusplus
}
#endif

#endif

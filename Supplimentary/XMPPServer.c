#include "XMPPServer.h"

#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define BUFFER_ALLIGNMENT   2048
#define INBOUND_LENGTH      2048
#define INBOUND_COUNT       2048
#define POLL_INTERVAL       5000  // milliseconds
#define CONNECTION_TIMEOUT  60    // seconds

// Helpers

static void MoveScratchPad(const char** store, char* old, char* new, int count)
{
  int index;
  ptrdiff_t delta;

  if ((old != new) &&
      (old != NULL))
  {
    index = 0;
    delta = new - old;

    while (index < count)
    {
      store[index] += (store[index] != NULL) * delta;
      index        ++;
    }
  }
}

static char* AppendScratchPad(struct XMPPScratchPad* scratch, const char** store, int count, const xmlChar* value, int length, int append)
{
  char* data;
  size_t size;

  if (length < 0)
  {
    //
    length = xmlStrlen(value);
  }

  size = scratch->length + length + 1;

  if (scratch->size < size)
  {
    size = (size + BUFFER_ALLIGNMENT - 1) & ~(BUFFER_ALLIGNMENT - 1);
    data = (char*)realloc(scratch->data, size);

    if (data == NULL)
    {
      //
      return NULL;
    }

    MoveScratchPad(store, scratch->data, data, count);

    scratch->data = data;
    scratch->size = size;
  }

  data             = scratch->data + scratch->length - append;
  data[length]     = '\0';
  scratch->length += length - append + 1;

  memcpy(data, value, length);
  return data;
}

static void ClearStore(struct XMPPConnection* connection)
{
  memset(connection->store, 0, sizeof(const char*) * XMPP_STORE_LENGTH);
  connection->scratch.length = 0;
}

static void AppendStore(struct XMPPConnection* connection, int index, const xmlChar* value, int length, int append)
{
  connection->state |= XMPP_STATE_FAILURE *
    ((append                   != 0)    &&
     (connection->store[index] != NULL) &&
     (AppendScratchPad(&connection->scratch, connection->store, XMPP_STORE_LENGTH, value, length, 1) == NULL) ||
     ((append                   == 0)     ||
      (connection->store[index] == NULL)) &&
     ((connection->store[index]  = AppendScratchPad(&connection->scratch, connection->store, XMPP_STORE_LENGTH, value, length, 0)) == NULL));
}

// SAX handler

static void HandleXMLStartElement(void* context, const xmlChar* name, const xmlChar* prefix, const xmlChar* location, int count1, const xmlChar** namespaces, int count2, int count3, const xmlChar** attributes)
{
  struct XMPPConnection* connection;
  union XMPPEventData parameter;
  struct XMPPServer* server;
  const xmlChar* value;
  int length;
  int index;

  connection = (struct XMPPConnection*)context;
  server     = connection->server;

  connection->depth ++;

  if (connection->depth == 1)
  {
    value = NULL;

    for (index = 0; index < count1; ++ index)
    {
      if ((namespaces[index * 2 + 0] == NULL) &&
          (namespaces[index * 2 + 1] != NULL))
      {
        value = namespaces[index * 2 + 1];
        break;
      }
    }

    if ((prefix   == NULL) ||
        (value    == NULL) ||
        (location == NULL) ||
        (xmlStrcmp(prefix,   BAD_CAST "stream") != 0) ||
        (xmlStrcmp(name,     BAD_CAST "stream") != 0) ||
        (xmlStrcmp(value,    BAD_CAST "jabber:client") != 0) ||
        (xmlStrcmp(location, BAD_CAST "http://etherx.jabber.org/streams") != 0))
    {
      connection->state |= XMPP_STATE_FAILURE;
      return;
    }

    ClearStore(connection);
    AppendStore(connection, XMPP_STORE_STANZA_NAME, name, -1, 0);

    for (index = 0; index < count2; ++ index)
    {
      if ((attributes[index * 5 + 4] != NULL)  &&
          (name    = attributes[index * 5 + 0]) &&
          (value   = attributes[index * 5 + 3]) &&
          ((length = attributes[index * 5 + 4] - value) >= 0))
      {
        if (xmlStrcmp(name, BAD_CAST "id")   == 0)  AppendStore(connection, XMPP_STORE_STANZA_ID,   value, length, 0);
        if (xmlStrcmp(name, BAD_CAST "to")   == 0)  AppendStore(connection, XMPP_STORE_STANZA_TO,   value, length, 0);
        if (xmlStrcmp(name, BAD_CAST "from") == 0)  AppendStore(connection, XMPP_STORE_STANZA_FROM, value, length, 0);
      }
    }

    parameter.store    = connection->store;
    connection->state |= XMPP_STATE_GUARD;
    server->function(server, connection, XMPP_EVENT_STREAM_BEGIN, &parameter);
    connection->state &= ~XMPP_STATE_GUARD;
    return;
  }

  if (connection->depth == 2)
  {
    ClearStore(connection);
    AppendStore(connection, XMPP_STORE_STANZA_NAME, name, -1, 0);

    for (index = 0; index < count2; ++ index)
    {
      if ((attributes[index * 5 + 4] != NULL)  &&
          (name    = attributes[index * 5 + 0]) &&
          (value   = attributes[index * 5 + 3]) &&
          ((length = attributes[index * 5 + 4] - value) >= 0))
      {
        if (xmlStrcmp(name, BAD_CAST "id")   == 0)  AppendStore(connection, XMPP_STORE_STANZA_ID,   value, length, 0);
        if (xmlStrcmp(name, BAD_CAST "to")   == 0)  AppendStore(connection, XMPP_STORE_STANZA_TO,   value, length, 0);
        if (xmlStrcmp(name, BAD_CAST "from") == 0)  AppendStore(connection, XMPP_STORE_STANZA_FROM, value, length, 0);
        if (xmlStrcmp(name, BAD_CAST "type") == 0)  AppendStore(connection, XMPP_STORE_STANZA_TYPE, value, length, 0);
      }
    }

    parameter.store    = connection->store;
    connection->state |= XMPP_STATE_GUARD;
    server->function(server, connection, XMPP_EVENT_STANZA_BEGIN, &parameter);
    connection->state &= ~XMPP_STATE_GUARD;
    return;
  }

  if ((connection->depth == 3)    &&
      (location          != NULL) &&
      (connection->store[XMPP_STORE_STANZA_NAME]    != NULL) &&
      (connection->store[XMPP_STORE_CHILD_LOCATION] == NULL) &&
      ((strcmp(connection->store[XMPP_STORE_STANZA_NAME], "iq")      == 0) ||
       (strcmp(connection->store[XMPP_STORE_STANZA_NAME], "message") == 0)))
  {
    AppendStore(connection, XMPP_STORE_CHILD_NAME,     name,     -1, 0);
    AppendStore(connection, XMPP_STORE_CHILD_LOCATION, location, -1, 0);
    return;
  }

  if ((connection->depth == 5) &&
      (connection->store[XMPP_STORE_STANZA_NAME]    != NULL) &&
      (connection->store[XMPP_STORE_CHILD_LOCATION] != NULL) &&
      (strcmp(connection->store[XMPP_STORE_STANZA_NAME],    "message") == 0) &&
      (strcmp(connection->store[XMPP_STORE_CHILD_LOCATION], "http://www.jivesoftware.com/xmlns/xmpp/properties") == 0))
  {
    connection->state &= ~(XMPP_STATE_HANDLE_NAME | XMPP_STATE_HANDLE_VALUE);
    connection->state |=
      XMPP_STATE_HANDLE_NAME  * (xmlStrcmp(name, BAD_CAST "name")  == 0) |
      XMPP_STATE_HANDLE_VALUE * (xmlStrcmp(name, BAD_CAST "value") == 0);
    return;
  }
}

static void HandleXMLEndElement(void* context, const xmlChar* name, const xmlChar* prefix, const xmlChar* location)
{
  struct XMPPConnection* connection;
  union XMPPEventData parameter;
  struct XMPPServer* server;
  int event;

  connection = (struct XMPPConnection*)context;
  server     = connection->server;

  connection->depth --;

  if (connection->depth <= 0)
  {
    connection->state |= XMPP_STATE_GUARD;
    server->function(server, connection, XMPP_EVENT_STREAM_END, NULL);
    connection->state &= ~XMPP_STATE_GUARD;
    return;
  }

  if (connection->depth == 1)
  {
    parameter.store    = connection->store;
    connection->state |= XMPP_STATE_GUARD;
    server->function(server, connection, XMPP_EVENT_STANZA_END, &parameter);
    connection->state &= ~XMPP_STATE_GUARD;
    return;
  }

  if ((connection->depth == 3) &&
      (connection->store[XMPP_STORE_STANZA_NAME]    != NULL) &&
      (connection->store[XMPP_STORE_CHILD_LOCATION] != NULL) &&
      (strcmp(connection->store[XMPP_STORE_STANZA_NAME],    "message") == 0) &&
      (strcmp(connection->store[XMPP_STORE_CHILD_LOCATION], "http://www.jivesoftware.com/xmlns/xmpp/properties") == 0))

  {
    connection->state                            &= ~(XMPP_STATE_HANDLE_NAME | XMPP_STATE_HANDLE_VALUE);
    connection->store[XMPP_STORE_CHILD_PROPERTY]  = NULL;
    return;
  }
}

static void HandleXMLCharacters(void* context, const xmlChar* text, int length)
{
  struct XMPPConnection* connection;

  connection = (struct XMPPConnection*)context;

  if ((connection->depth == 5) &&
      (connection->state & XMPP_STATE_HANDLE_NAME))
  {
    AppendStore(connection, XMPP_STORE_CHILD_PROPERTY, text, length, 1);
    return;
  }

  if ((connection->depth == 5) &&
      (connection->state & XMPP_STATE_HANDLE_VALUE) &&
      (connection->store[XMPP_STORE_CHILD_PROPERTY] != NULL))
  {
    if (strcmp(connection->store[XMPP_STORE_CHILD_PROPERTY], "MsgType")    == 0)  AppendStore(connection, XMPP_STORE_MESSAGE_TYPE, text, length, 1);
    if (strcmp(connection->store[XMPP_STORE_CHILD_PROPERTY], "MsgText")    == 0)  AppendStore(connection, XMPP_STORE_MESSAGE_TEXT, text, length, 1);
    if (strcmp(connection->store[XMPP_STORE_CHILD_PROPERTY], "ExtendInfo") == 0)  AppendStore(connection, XMPP_STORE_MESSAGE_INFO, text, length, 1);
    return;
  }
}

static void HandleXMLWrror(void* context, const char* format, ...)
{
  struct XMPPConnection* connection;

  connection         = (struct XMPPConnection*)context;
  connection->state |= XMPP_STATE_FAILURE;
}

// Connection tracking

static void DestroyConnection(struct XMPPServer* server, struct XMPPConnection* connection);

static void HandleSocket(struct FastSocket* socket, int event, int parameter)
{
  int result;
  struct FastBuffer* buffer;
  struct XMPPConnection* connection;

  connection = (struct XMPPConnection*)socket->closure;
  result     = (event & POLLHUP) || (event & POLLERR);

  if (event & POLLIN)
  {
    if (connection->depth >= 1)
    {
      // Update valid connection only
      clock_gettime(CLOCK_MONOTONIC, &connection->time);
    }

    while ((result == 0) &&
           (buffer  = ReceiveFastSocketBuffer(socket)))
    {
      result = xmlParseChunk(connection->parser, buffer->data, buffer->length, 0);
      ReleaseFastBuffer(buffer);
    }
  }

  if ((result != 0) ||
      (connection->state & XMPP_STATE_CLOSE) ||
      (connection->state & XMPP_STATE_FAILURE))
  {
    connection->state |= XMPP_STATE_FAILURE;
    DestroyConnection(connection->server, connection);
  }
}

static int CreateConnection(struct XMPPServer* server, int handle, struct sockaddr* address, socklen_t length)
{
  struct XMPPConnection* connection;
  struct XMPPConnection* linked;

  if ((connection         = (struct XMPPConnection*)calloc(1, sizeof(struct XMPPConnection))) &&
      (connection->parser = xmlCreatePushParserCtxt(&server->handler, connection, NULL, 0, NULL)) &&
      (connection->socket = CreateFastSocket(server->ring, server->provider, server->inbound, server->outbound, handle, NULL, 0, FASTSOCKET_MODE_ZERO_COPY, 0, HandleSocket, connection)))
  {
    if (linked = server->connections)
    {
      connection->next = linked;
      linked->previous = connection;
    }

    server->connections = connection;
    connection->server  = server;

    clock_gettime(CLOCK_MONOTONIC, &connection->time);
    memcpy(&connection->outcome, &connection->time, sizeof(struct timespec));
    memcpy(&connection->address, address, length);

    xmlCtxtUseOptions(connection->parser, XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    return 0;
  }

  if (connection != NULL)
  {
    xmlFreeParserCtxt(connection->parser);
    ReleaseFastSocket(connection->socket);
    free(connection);
  }

  return -1;
}

static void DestroyConnection(struct XMPPServer* server, struct XMPPConnection* connection)
{
  struct XMPPConnection* linked;

  if (~connection->state & XMPP_STATE_GUARD)
  {
    if (linked = connection->next)      linked->previous    = connection->previous;
    if (linked = connection->previous)  linked->next        = connection->next;
    else                                server->connections = connection->next;

    if ((~connection->state & XMPP_STATE_CLOSE) &&
        (~connection->state & XMPP_STATE_DESTROY))
    {
      connection->state |= XMPP_STATE_GUARD;
      server->function(server, connection, XMPP_EVENT_CONNECTION_DESTROY, NULL);
    }

    xmlParseChunk(connection->parser, NULL, 0, 1);
    xmlFreeParserCtxt(connection->parser);
    ReleaseFastSocket(connection->socket);
    free(connection->scratch.data);
    free(connection);
  }
}

static void RejectConnection(struct XMPPServer* server, int handle)
{
  struct FastRingDescriptor* descriptor;

  if (descriptor = AllocateFastRingDescriptor(server->ring, NULL, NULL))
  {
    io_uring_prep_close(&descriptor->submission, handle);
    SubmitFastRingDescriptor(descriptor, 0);
    return;
  }

  close(handle);
}

static int HandleConnection(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  union XMPPEventData parameter;
  struct XMPPServer* server;

  if ((completion != NULL) &&
      (server      = (struct XMPPServer*)descriptor->closure))
  {
    parameter.address = (struct sockaddr*)&descriptor->data.socket.address;
    if ((completion->res >= 0) &&
        ((server->function(server, NULL, XMPP_EVENT_CONNECTION_ACCEPT, &parameter) < 0) ||
         (CreateConnection(server, completion->res, (struct sockaddr*)&descriptor->data.socket.address, descriptor->data.socket.length) < 0)))
    {
      //
      RejectConnection(server, completion->res);
    }

    descriptor->data.socket.length = sizeof(struct sockaddr_storage);
    SubmitFastRingDescriptor(descriptor, 0);
    return 1;
  }

  return 0;
}

static void HandleTimeout(struct FastRingDescriptor* descriptor)
{
  struct XMPPConnection* connection;
  struct XMPPConnection* next;
  struct XMPPServer* server;
  struct timespec time;

  server     = (struct XMPPServer*)descriptor->closure;
  connection = server->connections;

  clock_gettime(CLOCK_MONOTONIC, &time);

  while (connection != NULL)
  {
    next = connection->next;

    if (time.tv_sec >= (connection->time.tv_sec + CONNECTION_TIMEOUT))
    {
      connection->state |= XMPP_STATE_TIMEOUT;
      DestroyConnection(server, connection);
    }

    connection = next;
  }
}

struct XMPPServer* CreateXMPPServer(struct FastRing* ring, HandleXMPPEvent function, void* closure, int port)
{
  struct FastRingDescriptor* descriptor;
  struct sockaddr_in6 address;
  struct XMPPServer* server;
  int value;

  if (server = (struct XMPPServer*)calloc(1, sizeof(struct XMPPServer)))
  {
    memset(&address, 0, sizeof(struct sockaddr_in6));

    address.sin6_family = AF_INET6;
    address.sin6_port   = htons(port);
    value               = 1;

    server->handle = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);

    if ((server->handle < 0) ||
        (setsockopt(server->handle, SOL_SOCKET,  SO_REUSEADDR,     &value, sizeof(int)) < 0) ||
        (setsockopt(server->handle, SOL_SOCKET,  SO_REUSEPORT,     &value, sizeof(int)) < 0) ||
        (setsockopt(server->handle, SOL_TCP,     TCP_NODELAY,      &value, sizeof(int)) < 0) ||
        (setsockopt(server->handle, IPPROTO_TCP, TCP_DEFER_ACCEPT, &value, sizeof(int)) < 0) ||
        (bind(server->handle, (struct sockaddr*)&address, sizeof(struct sockaddr_in6))  < 0) ||
        (listen(server->handle, 256) < 0))
    {
      close(server->handle);
      free(server);
      return NULL;
    }

    server->ring     = ring;
    server->closure  = closure;
    server->function = function;

    server->handler.initialized    = XML_SAX2_MAGIC;
    server->handler.startElementNs = HandleXMLStartElement;
    server->handler.endElementNs   = HandleXMLEndElement;
    server->handler.characters     = HandleXMLCharacters;
    server->handler.error          = HandleXMLWrror;

    server->inbound  = CreateFastBufferPool(ring);
    server->outbound = CreateFastBufferPool(ring);
    server->provider = CreateFastRingBufferProvider(ring, 0, INBOUND_COUNT, INBOUND_LENGTH, AllocateRingFastBuffer, server->inbound);
    server->listner  = AllocateFastRingDescriptor(ring, HandleConnection, server);
    server->timeout  = SetFastRingTimeout(ring, NULL, POLL_INTERVAL, TIMEOUT_FLAG_REPEAT, HandleTimeout, server);

    if (descriptor = server->listner)
    {
      descriptor->data.socket.length = sizeof(struct sockaddr_storage);
      io_uring_prep_accept(&descriptor->submission, server->handle, (struct sockaddr*)&descriptor->data.socket.address, &descriptor->data.socket.length, 0);
      SubmitFastRingDescriptor(descriptor, 0);
    }
  }

  return server;
}

void ReleaseXMPPServer(struct XMPPServer* server)
{
  struct FastRingDescriptor* descriptor;
  struct XMPPConnection* connection;

  if (server != NULL)
  {
    while (connection = server->connections)
    {
      connection->state |= XMPP_STATE_DESTROY;
      DestroyConnection(server, connection);
    }

    if (descriptor = server->listner)
    {
      descriptor->function = NULL;
      descriptor->closure  = NULL;
    }

    SetFastRingTimeout(server->ring, server->timeout, -1, 0, NULL, NULL);
    ReleaseFastRingBufferProvider(server->provider, ReleaseRingFastBuffer);
    ReleaseFastBufferPool(server->outbound);
    ReleaseFastBufferPool(server->inbound);
    close(server->handle);
    free(server);
  }
}

void CloseXMPPConnection(struct XMPPConnection* connection)
{
  if (connection != NULL)
  {
    connection->closure  = NULL;
    connection->state   |= XMPP_STATE_CLOSE;
    DestroyConnection(connection->server, connection);
  }
}

int SendMPPConnection(struct XMPPConnection* connection, const char* format, ...)
{
  va_list arguments;
  int result;

  va_start(arguments, format);
  result = SendVariadicXMPPConnection(connection, format, arguments);
  va_end(arguments);

  return result;
}

int SendVariadicXMPPConnection(struct XMPPConnection* connection, const char* format, va_list arguments)
{
  struct FastRingDescriptor* descriptor;
  struct FastSocket* socket;
  struct XMPPServer* server;
  struct FastBuffer* buffer;
  va_list copy;
  int length;

  socket     = connection->socket;
  server     = connection->server;
  buffer     = NULL;
  descriptor = NULL;

  va_copy(copy, arguments);
  length = vsnprintf(NULL, 0, format, arguments);

  if ((length > 0) &&
      (buffer     = AllocateFastBuffer(server->outbound, length + 1, 0)) &&
      (descriptor = AllocateFastRingDescriptor(server->ring, NULL, NULL)))
  {
    length = vsnprintf((char*)buffer->data, buffer->size, format, copy);
    io_uring_prep_send_zc(&descriptor->submission, socket->handle, buffer->data, length, 0, 0);
  }

  va_end(copy);
  return TransmitFastSocketDescriptor(socket, descriptor, buffer);
}

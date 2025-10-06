#include "AAAClient.h"

#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>

#include "RADIUSTools.h"

#define INBOUND_COUNT   128
#define INBOUND_LENGTH  2048

static void TransmitOutboundMessage(struct AAAClient* client, struct AAAMessage* message)
{
  struct FastSocket* socket;
  struct FastBuffer* buffer;
  struct FastRingDescriptor* descriptor;

  socket = client->socket;
  buffer = message->buffer;

  HoldFastBuffer(buffer);

  if (descriptor = AllocateFastRingDescriptor(client->ring, NULL, NULL))
  {
    io_uring_prep_send_zc(&descriptor->submission, socket->handle, buffer->data, buffer->length, 0, 0);
    io_uring_prep_send_set_addr(&descriptor->submission, message->address, message->length);
  }

  TransmitFastSocketDescriptor(client->socket, descriptor, buffer);
}

static void HandleInboundMessage(struct AAAClient* client, struct sockaddr* address, struct RADIUSDataUnit* unit)
{
  struct FastBuffer* buffer;
  struct AAAMessage* message;

  message = client->messages + unit->identifier;

  if ((unit->code      == RADIUS_CODE_ACCT_RESP) &&
      (buffer           = message->buffer)       &&
      (buffer->data[0] == RADIUS_CODE_ACCT_REQ)  &&
      (CheckRADIUSAuthenticator(unit, buffer->data + 4, message->secret) == 0))
  {
    ReleaseFastBuffer(buffer);
    SetFastRingTimeout(client->ring, message->descriptor, -1, 0, NULL, NULL);
    message->buffer     = NULL;
    message->descriptor = NULL;
  }
}

static void HandleTimeoutEvent(struct FastRingDescriptor* descriptor)
{
  struct AAAClient* client;
  struct AAAMessage* message;

  message = (struct AAAMessage*)descriptor->closure;

  if (-- message->count)
  {
    client              = (struct AAAClient*)message->client;
    message->descriptor = SetFastRingTimeout(client->ring, NULL, client->interval, 0, HandleTimeoutEvent, message);
    TransmitOutboundMessage(client, message);
  }
  else
  {
    ReleaseFastBuffer(message->buffer);
    message->buffer     = NULL;
    message->descriptor = NULL;
  }
}

static void HandleSocketEvent(struct FastSocket* socket, int event, int parameter)
{
  int length;
  struct sockaddr* address;
  struct AAAClient* client;
  struct FastBuffer* buffer;
  struct RADIUSDataUnit* data;
  struct io_uring_recvmsg_out* output;

  client = (struct AAAClient*)socket->closure;

  while (buffer = ReceiveFastSocketBuffer(socket))
  {
    output  = io_uring_recvmsg_validate(buffer->data, buffer->length, &client->message);
    address = io_uring_recvmsg_name(output);
    data    = (struct RADIUSDataUnit*)io_uring_recvmsg_payload(output, &client->message);
    length  = io_uring_recvmsg_payload_length(output, buffer->length, &client->message);

    if ((length >= sizeof(struct RADIUSDataUnit)) &&
        (length == be16toh(data->length)))
    {
      //
      HandleInboundMessage(client, address, data);
    }

    ReleaseFastBuffer(buffer);
  }
}

uint8_t GetAAAClientMessageID(struct AAAClient* client)
{
  uint8_t identifier;

  identifier = client->identifier;

  while ((identifier != ++ client->identifier) &&
         (client->messages[client->identifier].buffer != NULL));

  return client->identifier;
}

void SubmitAAAClientMessage(struct AAAClient* client, struct FastBuffer* buffer, struct sockaddr* address, const char* secret)
{
  struct AAAMessage* message;

  message = client->messages + buffer->data[1];

  ReleaseFastBuffer(message->buffer);

  message->address    = address;
  message->buffer     = buffer;
  message->secret     = secret;
  message->client     = client;
  message->count      = client->count;
  message->descriptor = SetFastRingTimeout(client->ring, message->descriptor, client->interval, 0, HandleTimeoutEvent, message);

  switch (address->sa_family)
  {
    case AF_INET:  message->length = sizeof(struct sockaddr_in);   break;
    case AF_INET6: message->length = sizeof(struct sockaddr_in6);  break;        
  }

  TransmitOutboundMessage(client, message);
}

struct AAAClient* CreateAAAClient(struct FastRing* ring, int count, int interval)
{
  struct AAAClient* client;

  int handle;
  struct addrinfo hint;
  struct addrinfo* information;

  memset(&hint, 0, sizeof(struct addrinfo));

  handle           = -1;
  hint.ai_family   = AF_UNSPEC;
  hint.ai_socktype = SOCK_DGRAM;
  hint.ai_flags    = AI_PASSIVE | AI_NUMERICSERV;

  if ((getaddrinfo(NULL, "0", &hint, &information) != 0) ||
      ((handle = socket(information->ai_family, information->ai_socktype | SOCK_CLOEXEC, information->ai_protocol)) < 0) ||
      (bind(handle, information->ai_addr, information->ai_addrlen) < 0))
  {
    freeaddrinfo(information);
    close(handle);
    return NULL;
  }

  freeaddrinfo(information);

  client = (struct AAAClient*)calloc(1, sizeof(struct AAAClient));

  client->ring                = ring;
  client->count               = count;
  client->interval            = interval;
  client->message.msg_namelen = sizeof(struct sockaddr_in6);
  client->pool                = CreateFastBufferPool(ring);
  client->provider            = CreateFastRingBufferProvider(ring, 0, INBOUND_COUNT, INBOUND_LENGTH, AllocateRingFastBuffer, client->pool);
  client->socket              = CreateFastSocket(ring, client->provider, client->pool, client->pool, handle, &client->message, 0, FASTSOCKET_MODE_ZERO_COPY, 0, HandleSocketEvent, client);

  return client;
}

void ReleaseAAAClient(struct AAAClient* client)
{
  int number;
  struct AAAMessage* message;

  if (client != NULL)
  {
    for (number = 0; number <= UINT8_MAX; number ++)
    {
      message = client->messages + number;
      ReleaseFastBuffer(message->buffer);
      SetFastRingTimeout(client->ring, message->descriptor, -1, 0, NULL, NULL);
    }

    ReleaseFastSocket(client->socket);
    ReleaseFastRingBufferProvider(client->provider, ReleaseRingFastBuffer);
    ReleaseFastBufferPool(client->pool);
    free(client);
  }
}

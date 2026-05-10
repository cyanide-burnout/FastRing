#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <openssl/err.h>

#include "FastRing.h"
#include "Resolver.h"
#include "SSLSocket.h"

#define HTTPS_HOST       "google.com"
#define HTTPS_PORT       "443"
#define RING_LENGTH      0
#define BUFFER_COUNT     64
#define BUFFER_LENGTH    FASTBIO_BUFFER_SIZE
#define OUTBOUND_LIMIT   64

#define CLIENT_STATE_RUNNING  0
#define CLIENT_STATE_DONE     1

static const char request[] =
  "GET / HTTP/1.1\r\n"
  "Host: " HTTPS_HOST "\r\n"
  "User-Agent: SSLSocket/1.0\r\n"
  "Accept: */*\r\n"
  "Connection: close\r\n"
  "\r\n";

struct SSLClient
{
  struct FastRing* ring;
  struct FastBufferPool* pool;
  struct FastRingBufferProvider* provider;
  struct ResolverState* resolver;
  struct SSLSocket* socket;
  SSL_CTX* context;
};

static atomic_int state;

static void StopClient(void)
{
  atomic_store_explicit(&state, CLIENT_STATE_DONE, memory_order_relaxed);
}

static void HandleSignal(int signal)
{
  StopClient();
}

static int HandleSocketEvent(void* closure, SSL* connection, int event, int parameter, void* data)
{
  int result;
  size_t length;
  uint8_t buffer[BUFFER_LENGTH];
  struct SSLClient* client;

  client = (struct SSLClient*)closure;

  switch (event)
  {
    case SSL_EVENT_GREETED:
      // The verify callback must return OpenSSL preverify result
      return parameter;

    case SSL_EVENT_CONNECTED:
      printf("Connected to https://%s/\n", HTTPS_HOST);
      return TransmitSSLSocketData(client->socket, request, sizeof(request) - 1);

    case SSL_EVENT_RECEIVED:
      length = 0;

      IterateSSLSocketData(connection, buffer, sizeof(buffer), length, result,
      {
        if (result > 0)
        {
          fwrite(buffer, 1, length, stdout);
          length = 0;
        }
      });

      return result;

    case SSL_EVENT_FAILED:
      printf("TLS error: %s (%lu)\n", ERR_error_string(parameter, NULL), (unsigned long)parameter);
      printf("Verify error: %s\n", X509_verify_cert_error_string(SSL_get_verify_result(connection)));
      ReleaseSSLSocket(client->socket);
      client->socket = NULL;
      StopClient();
      return 0;

    case SSL_EVENT_DISCONNECTED:
      ReleaseSSLSocket(client->socket);
      client->socket = NULL;
      StopClient();
      return 0;
  }

  return 0;
}

static void ConnectAddress(struct SSLClient* client, const struct ares_addrinfo_node* node)
{
  int handle;
  struct SSLSocket* transport;

  handle = socket(node->ai_family, node->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, node->ai_protocol);

  if ((connect(handle, node->ai_addr, node->ai_addrlen) == 0) ||
      (errno  == EINPROGRESS))
  {
    printf("Connecting to %s:%s\n", HTTPS_HOST, HTTPS_PORT);

    if (transport = CreateSSLSocket(client->ring, client->provider, client->pool, client->pool, client->context, handle, SSL_ROLE_CLIENT, SSL_VERIFY_PEER | SSL_OP_ENABLE_KTLS, BUFFER_LENGTH, OUTBOUND_LIMIT, HandleSocketEvent, client))
    {
      SSL_set_tlsext_host_name(transport->connection, HTTPS_HOST);
      SSL_set1_host(transport->connection, HTTPS_HOST);
      client->socket = transport;
      return;
    }

    printf("Cannot start TLS\n");
  }

  close(handle);
  StopClient();
}

static void HandleResolvedAddress(void* closure, int status, int timeouts, struct ares_addrinfo* result)
{
  struct SSLClient* client;
  struct ares_addrinfo_node* node;

  client = (struct SSLClient*)closure;

  if ((status != ARES_SUCCESS) ||
      (result == NULL))
  {
    printf("Resolve failed: %s\n", (status == ARES_SUCCESS) ? "empty response" : ares_strerror(status));
    StopClient();
    return;
  }

  printf("Resolved %s\n", HTTPS_HOST);

  for (node = result->nodes; node != NULL; node = node->ai_next)
  {
    if ((node->ai_family == AF_INET) ||
        (node->ai_family == AF_INET6))
    {
      ConnectAddress(client, node);

      if ((client->socket != NULL) ||
          (atomic_load_explicit(&state, memory_order_relaxed) != CLIENT_STATE_RUNNING))
      {
        ares_freeaddrinfo(result);
        return;
      }
    }
  }

  printf("Connect failed\n");
  ares_freeaddrinfo(result);
  StopClient();
}

static void ResolveHost(struct SSLClient* client)
{
  struct ResolverState* resolver;
  struct ares_addrinfo_hints hint;

  printf("Resolving %s\n", HTTPS_HOST);

  memset(&hint, 0, sizeof(hint));

  resolver         = client->resolver;
  hint.ai_flags    = ARES_AI_NUMERICSERV;
  hint.ai_family   = AF_UNSPEC;
  hint.ai_socktype = SOCK_STREAM;
  hint.ai_protocol = IPPROTO_TCP;

  ares_getaddrinfo(resolver->channel, HTTPS_HOST, HTTPS_PORT, &hint, HandleResolvedAddress, client);
  UpdateResolverTimer(resolver);
}

int main()
{
  struct sigaction action;
  struct SSLClient client;

  memset(&client, 0, sizeof(client));
  atomic_store_explicit(&state, CLIENT_STATE_RUNNING, memory_order_relaxed);

  action.sa_handler = HandleSignal;
  action.sa_flags   = SA_NODEFER | SA_RESTART;

  sigemptyset(&action.sa_mask);

  sigaction(SIGHUP,  &action, NULL);
  sigaction(SIGINT,  &action, NULL);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGQUIT, &action, NULL);

  if ((ares_library_init(ARES_LIB_INIT_ALL) != ARES_SUCCESS) ||
      ((client.ring     = CreateFastRing(RING_LENGTH)) == NULL) ||
      ((client.pool     = CreateFastBufferPool(client.ring)) == NULL) ||
      ((client.provider = CreateFastRingBufferProvider(client.ring, 0, BUFFER_COUNT, BUFFER_LENGTH, AllocateRingFastBuffer, client.pool)) == NULL) ||
      ((client.resolver = CreateResolver(client.ring)) == NULL) ||
      ((client.context  = SSL_CTX_new(TLS_client_method())) == NULL))
  {
    printf("Initialization failed\n");
    StopClient();
  }
  else
  {
    SSL_CTX_set_default_verify_paths(client.context);
    ResolveHost(&client);
  }

  while ((atomic_load_explicit(&state, memory_order_relaxed) == CLIENT_STATE_RUNNING) &&
         (WaitForFastRing(client.ring, 200, NULL) >= 0));

  ReleaseSSLSocket(client.socket);
  ReleaseFastRingBufferProvider(client.provider, ReleaseRingFastBuffer);
  ReleaseFastBufferPool(client.pool);
  ReleaseResolver(client.resolver);
  SSL_CTX_free(client.context);
  ReleaseFastRing(client.ring);
  ares_library_cleanup();

  return 0;
}

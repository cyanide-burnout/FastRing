#define _GNU_SOURCE

#include <unistd.h>
#include <alloca.h>
#include <malloc.h>
#include <endian.h>
#include <bsd/string.h>

#include "H2OCore.h"

// Helpers

static void TransmitQUICToken(struct H2OCore* core, quicly_address_t* destination, quicly_address_t* source, quicly_decoded_packet_t* packet)
{
  uint8_t prefix;
  uint32_t length;
  struct iovec vector;
  quicly_cid_t identifier;
  ptls_aead_context_t** cache;

  core->quicly.cid_encryptor->encrypt_cid(core->quicly.cid_encryptor, &identifier, NULL, &core->identifier);

  switch (packet->version)
  {
    case QUICLY_PROTOCOL_VERSION_1:        cache = core->cache + 0;  break;
    case QUICLY_PROTOCOL_VERSION_DRAFT29:  cache = core->cache + 1;  break;
    case QUICLY_PROTOCOL_VERSION_DRAFT27:  cache = core->cache + 2;  break;
    default:                               cache = NULL;             break;
  }

  prefix          = 0;
  vector.iov_base = (uint8_t*)alloca(QUICLY_MIN_CLIENT_INITIAL_SIZE);
  vector.iov_len  = quicly_send_retry(core->server.super.quic, core->encryptor, packet->version,
    &source->sa, packet->cid.src, &destination->sa, ptls_iovec_init(identifier.cid, identifier.len),
    packet->cid.dest.encrypted, ptls_iovec_init(&prefix, 1), ptls_iovec_init(NULL, 0), cache, vector.iov_base);

  h2o_quic_send_datagrams(&core->server.super, source, destination, &vector, 1);
}

// H2O bindings

static void HandleClose(uv_handle_t* handle)
{
  // Dummy
}

static void HandleTCPConnection(uv_stream_t* tcp, int status)
{
  uv_tcp_t* connection;
  h2o_socket_t* socket;
  struct H2OCore* core;

  if (status == 0)
  {
    core       = (struct H2OCore*)tcp->data;
    connection = (uv_tcp_t*)malloc(sizeof(uv_tcp_t));

    uv_tcp_init(tcp->loop, connection);

    if (uv_accept(tcp, (uv_stream_t*)connection) == 0)
    {
      socket = h2o_uv_socket_create((uv_handle_t*)connection, (uv_close_cb)free);
      h2o_accept(&core->accept, socket);
      return;
    }

    uv_close((uv_handle_t*)connection, (uv_close_cb)free);
  }
}

static h2o_quic_conn_t* HandleQUICConnection(h2o_quic_ctx_t* quic, quicly_address_t* destination, quicly_address_t* source, quicly_decoded_packet_t* packet)
{
  const char *error;
  struct H2OCore* core;
  h2o_http3_conn_t* connection;
  quicly_address_token_plaintext_t* token;

  core  = (struct H2OCore*)((uint8_t*)quic - offsetof(struct H2OCore, server.super));
  token = NULL;
  error = NULL;

  if ((packet->token.len != 0) &&
      (token = (quicly_address_token_plaintext_t*)alloca(sizeof(quicly_address_token_plaintext_t))) &&
      ((quicly_decrypt_address_token(core->decryptor, token, packet->token.base, packet->token.len, 1, &error) != 0) ||
       (h2o_socket_compare_address(&source->sa, &token->remote.sa, token->type == QUICLY_ADDRESS_TOKEN_TYPE_RETRY) != 0)))
  {
    //
    token = NULL;
  }

  if ((core->server.send_retry) &&
      ((token       == NULL)    ||
       (token->type != QUICLY_ADDRESS_TOKEN_TYPE_RETRY)))
  {
    TransmitQUICToken(core, destination, source, packet);
    return NULL;
  }

  connection = h2o_http3_server_accept(&core->server, destination, source, packet, token, &H2O_HTTP3_CONN_CALLBACKS);

  if ((connection         != NULL) &&
      (connection         != &h2o_http3_accept_conn_closed) &&
      (&connection->super != &h2o_quic_accept_conn_decryption_failed))
  {
    /* cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_FLAGS="-DQUICLY_USE_TRACER=1" ..
    struct _st_quicly_conn_public_t* public = (struct _st_quicly_conn_public_t*)connection->super.quic;
    public->tracer.cb  = (quicly_trace_cb)fprintf;
    public->tracer.ctx = stderr;
    */
    return &connection->super;
  }

  return NULL;
}

static int HandleTLSHelloMessage(ptls_on_client_hello_t* handler, ptls_t* context, ptls_on_client_hello_parameters_t* parameters)
{
  return ptls_set_negotiated_protocol(context, (char*)H2O_STRLIT("h3"));
}

static const h2o_iovec_t both[]       = { H2O_STRLIT("h2"), H2O_STRLIT("http/1.1"), { NULL, 0 } };
static const h2o_iovec_t h2[]         = { H2O_STRLIT("h2"),                         { NULL, 0 } };
static const h2o_iovec_t http1[]      = { H2O_STRLIT("http/1.1"),                   { NULL, 0 } };
static const h2o_iovec_t* protocols[] = { NULL, both, h2, http1 };

struct H2OCore* CreateH2OCore(uv_loop_t* loop, const struct sockaddr* address, SSL_CTX* context1, ptls_context_t* context2, struct H2ORoute* route, int options)
{
  int value;
  int handle;
  struct H2OCore* core;
  h2o_hostconf_t* host;
  h2o_pathconf_t* path;
  struct H2OHandler* handler;

  if (core = (struct H2OCore*)calloc(1, sizeof(struct H2OCore)))
  {
    uv_tcp_init(loop, &core->tcp);
    core->tcp.data = core;

    if (uv_tcp_nodelay(&core->tcp, 1)       ||
        uv_tcp_bind(&core->tcp, address, 0) ||
        uv_listen((uv_stream_t*)&core->tcp, 128, HandleTCPConnection))
    {
      uv_close((uv_handle_t*)&core->tcp, NULL);
      core->state |= H2OCORE_STATE_TCP_FAILED;
    }

    h2o_config_init(&core->global);

    host = h2o_config_register_host(&core->global, h2o_iovec_init(H2O_STRLIT("default")), 65535);

    while (route->path != NULL)
    {
      path    = h2o_config_register_path(host, route->path, route->flags);
      handler = (struct H2OHandler*)h2o_create_handler(path, sizeof(struct H2OHandler));

      handler->closure                          = route->closure;
      handler->super.on_req                     = route->function;
      handler->super.supports_request_streaming = !!(route->options & H2OCORE_ROUTE_OPTION_STREAMING);

      route ++;
    }

    h2o_context_init(&core->context, loop, &core->global);

    core->accept.ctx     = &core->context;
    core->accept.hosts   = core->global.hosts;
    core->accept.ssl_ctx = context1;

    if ((context1 != NULL) &&
        (options & 3))
    {
      // Set H2OCORE_ROUTE_OPTION_APLN_*
      h2o_ssl_register_alpn_protocols(core->accept.ssl_ctx, protocols[options & 3]);
    }

    if (context2 != NULL)
    {
      value  = 1;
      handle = socket(address->sa_family, SOCK_DGRAM | SOCK_NONBLOCK, 0);

      if ((handle < 0) ||
          (setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(int)) < 0) ||
          (address->sa_family == AF_INET)  && ((setsockopt(handle, IPPROTO_IP,   IP_PKTINFO,       &value, sizeof(int)) < 0) || (bind(handle, address, sizeof(struct sockaddr_in))  < 0)) ||
          (address->sa_family == AF_INET6) && ((setsockopt(handle, IPPROTO_IPV6, IPV6_RECVPKTINFO, &value, sizeof(int)) < 0) || (bind(handle, address, sizeof(struct sockaddr_in6)) < 0)))
      {
        close(handle);
        core->state |= H2OCORE_STATE_UDP_FAILED;
      }

      ptls_openssl_random_bytes(core->secret, PTLS_MAX_SECRET_SIZE);

      core->udp                  = h2o_uv__poll_create(loop, handle, HandleClose);
      core->quicly               = quicly_spec_context;
      core->quicly.tls           = context2;
      core->quicly.cid_encryptor = quicly_new_default_cid_encryptor(&ptls_openssl_aes128ecb, &ptls_openssl_aes128ecb, &ptls_openssl_sha256, ptls_iovec_init(core->secret, PTLS_SHA256_DIGEST_SIZE));
      core->identifier.node_id   = gethostid();
      core->identifier.thread_id = gettid();
      core->handler.cb           = HandleTLSHelloMessage;

      quicly_amend_ptls_context(core->quicly.tls);
      h2o_http3_server_amend_quicly_context(&core->global, &core->quicly);
      h2o_http3_server_init_context(&core->context, &core->server.super, loop, core->udp, &core->quicly, &core->identifier, HandleQUICConnection, NULL, 0);

      context2->on_client_hello = &core->handler;
      core->server.accept_ctx   = &core->accept;

      if (core->server.send_retry = !!(options & H2OCORE_OPTION_H3_ENABLE_RETRY))
      {
        core->encryptor = ptls_aead_new(ptls_openssl_cipher_suites[0]->aead, ptls_openssl_cipher_suites[0]->hash, 1, core->secret, NULL);
        core->decryptor = ptls_aead_new(ptls_openssl_cipher_suites[0]->aead, ptls_openssl_cipher_suites[0]->hash, 0, core->secret, NULL);
      }
    }
  }

  return core;
}

void StopH2OCore(struct H2OCore* core)
{
  if (core != NULL)
  {
    if (core->udp               != NULL)  h2o_quic_dispose_context(&core->server.super);
    if (core->tcp.io_watcher.fd != -1)    uv_close((uv_handle_t*)&core->tcp, NULL);

    h2o_context_dispose(&core->context);
    h2o_config_dispose(&core->global);
  }
}

void ReleaseH2OCore(struct H2OCore* core)
{
  if (core != NULL)
  {
    if (core->quicly.cid_encryptor != NULL)  quicly_free_default_cid_encryptor(core->quicly.cid_encryptor);
    if (core->encryptor            != NULL)  ptls_aead_free(core->encryptor);
    if (core->decryptor            != NULL)  ptls_aead_free(core->decryptor);
    if (core->cache[0]             != NULL)  ptls_aead_free(core->cache[0]);
    if (core->cache[1]             != NULL)  ptls_aead_free(core->cache[1]);
    if (core->cache[2]             != NULL)  ptls_aead_free(core->cache[2]);
    free(core);
  }
}

void UpdateH2OCoreSecurity(struct H2OCore* core, SSL_CTX* context1, ptls_context_t* context2, int options)
{
  if ((core     != NULL) &&
      (context1 != NULL))
  {
    core->accept.ssl_ctx = context1;

    if ((context1 != NULL) &&
        (options & 3))
    {
      // Set H2OCORE_ROUTE_OPTION_APLN_*
      h2o_ssl_register_alpn_protocols(core->accept.ssl_ctx, protocols[options & 3]);
    }
  }

  if ((core     != NULL) &&
      (context2 != NULL))
  {
    core->quicly.tls          = context2;
    context2->on_client_hello = &core->handler;

    quicly_amend_ptls_context(core->quicly.tls);
  }
}

size_t GetH2OCoreConnectionCount(struct H2OCore* core)
{
  return (core != NULL) ? (core->context._conns.num_conns.idle + core->context._conns.num_conns.active + core->context._conns.num_conns.shutdown) : 0;
}


// Supplimentary routines

const char* GetH2OHeaderByIndex(const h2o_headers_t* headers, const h2o_token_t* token, size_t* size)
{
  ssize_t index;
  h2o_header_t* header;

  if ((index = h2o_find_header(headers, token, -1)) >= 0)
  {
    header = headers->entries + index;
    *size  = header->value.len;
    return (char*)header->value.base;
  }

  return NULL;
}

const char* GetH2OHeaderByName(const h2o_headers_t* headers, const char* name, size_t length, size_t* size)
{
  ssize_t index;
  h2o_header_t* header;

  if ((index = h2o_find_header_by_str(headers, name, length, -1)) >= 0)
  {
    header = headers->entries + index;
    *size  = header->value.len;
    return (char*)header->value.base;
  }

  return NULL;
}

int CompareH2OHeaderByIndex(const h2o_headers_t* headers, const h2o_token_t* token, const void* sample, size_t size)
{
  ssize_t index;
  h2o_header_t* header;

  if ((index = h2o_find_header(headers, token, -1)) >= 0)
  {
    header = headers->entries + index;
    return h2o_memis(header->value.base, header->value.len, sample, size);
  }

  return 0;
}

int CompareH2OHeaderByName(const h2o_headers_t* headers, const char* name, size_t length, const void* sample, size_t size)
{
  ssize_t index;
  h2o_header_t* header;

  if ((index = h2o_find_header_by_str(headers, name, length, -1)) >= 0)
  {
    header = headers->entries + index;
    return h2o_memis(header->value.base, header->value.len, sample, size);
  }

  return 0;
}

int HasInH2OHeaderByIndex(const h2o_headers_t* headers, const h2o_token_t* token, const char* needle)
{
  ssize_t index;
  h2o_header_t* header;

  if (index = h2o_find_header(headers, token, -1) >= 0)
  {
    header = headers->entries + index;
    return strnstr((char*)header->value.base, needle, header->value.len) != NULL;
  }

  return 0;
}

int HasInH2OHeaderByName(const h2o_headers_t* headers, const char* name, size_t length, const char* needle)
{
  ssize_t index;
  h2o_header_t* header;

  if ((index = h2o_find_header_by_str(headers, name, length, -1)) >= 0)
  {
    header = headers->entries + index;
    return strnstr((char*)header->value.base, needle, header->value.len) != NULL;
  }

  return 0;
}

void MakeH2OPercentEncodedString(h2o_iovec_t* vector, h2o_mem_pool_t* pool, const char* text, size_t length)
{
  static const char alphabet[] = "0123456789abcdef";
  char* data;

  data         = h2o_mem_alloc_pool(pool, char, length * 3 + 1);
  vector->base = data;
  vector->len  = 0;

  if (data != NULL)
  {
    while (length > 0)
    {
      if ((*text <= ' ') || (*text == '%') || ((uint8_t)*text >= 0x7f))
      {
        *(data ++) = '%';
        *(data ++) = alphabet[(uint8_t)*text >> 4];
        *(data ++) = alphabet[(uint8_t)*text & 15];
        vector->len += 3;
      }
      else
      {
        *(data ++) = *text;
        vector->len ++;
      }

      text   ++;
      length --;
    }
    *data = '\0';
  }
}

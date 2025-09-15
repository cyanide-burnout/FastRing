#define _GNU_SOURCE

#include <unistd.h>
#include <malloc.h>
#include <bsd/string.h>

#include "H2OCore.h"

static uint32_t GenerateMasterID(const ptls_iovec_t* certificate)
{
  // TODO

  return 1;
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
  struct H2OCore* core;
  h2o_http3_conn_t* connection;

  core       = (struct H2OCore*)((uint8_t*)quic - offsetof(struct H2OCore, server.super));
  connection = h2o_http3_server_accept(&core->server, destination, source, packet, NULL, &H2O_HTTP3_CONN_CALLBACKS);

  return (h2o_quic_conn_t*)connection;
}

struct H2OCore* CreateH2OCore(uv_loop_t* loop, const struct sockaddr* address, SSL_CTX* context1, ptls_context_t* context2, struct H2ORoute* route, int options)
{
  static const h2o_iovec_t both[]       = { H2O_STRLIT("h2"), H2O_STRLIT("http/1.1"), { NULL, 0 } };
  static const h2o_iovec_t h2[]         = { H2O_STRLIT("h2"),                         { NULL, 0 } };
  static const h2o_iovec_t http1[]      = { H2O_STRLIT("http/1.1"),                   { NULL, 0 } };
  static const h2o_iovec_t* protocols[] = { NULL, both, h2, http1 };

  struct H2OCore* core;
  h2o_hostconf_t* host;
  h2o_pathconf_t* path;
  h2o_socket_t* socket;
  struct H2OHandler* handler;

  if (core = (struct H2OCore*)calloc(1, sizeof(struct H2OCore)))
  {
    // Initialize UV layer

    uv_tcp_init(loop, &core->tcp);
    uv_udp_init(loop, &core->udp);

    core->tcp.data = core;

    if (uv_tcp_nodelay(&core->tcp, 1)       ||
        uv_tcp_bind(&core->tcp, address, 0) ||
        uv_listen((uv_stream_t*)&core->tcp, 128, HandleTCPConnection))
    {
      uv_close((uv_handle_t*)&core->tcp, NULL);
      core->state |= H2OCORE_STATE_TCP_FAILED;
    }

    if ((context1) &&
        (options & 3))
    {
      // Set H2OCORE_ROUTE_OPTION_APLN_*
      h2o_ssl_register_alpn_protocols(context1, protocols[options & 3]);
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

    if (context2 != NULL)
    {
      if (uv_udp_bind(&core->udp, address, UV_UDP_REUSEADDR))
      {
        uv_close((uv_handle_t*)&core->udp, NULL);
        core->state |= H2OCORE_STATE_UDP_FAILED;
      }

      socket = h2o_uv_socket_create((uv_handle_t*)&core->udp, NULL);

      memcpy(&core->quicly, &quicly_spec_context, sizeof(quicly_context_t));

      core->identifier.master_id = 1;
      core->identifier.node_id   = gethostid();
      core->identifier.thread_id = gettid();
      core->quicly.tls           = context2;

      if ((context2->certificates.count != 0) &&
          (context2->certificates.list  == NULL))
      {
        //
        core->identifier.master_id = GenerateMasterID(context2->certificates.list);
      }

      h2o_http3_server_amend_quicly_context(&core->global, &core->quicly);
      h2o_http3_server_init_context(&core->context, &core->server.super, loop, socket, &core->quicly, &core->identifier, HandleQUICConnection, NULL, 0);

      core->server.accept_ctx = &core->accept;
      core->server.send_retry = !!(options & H2OCORE_OPTION_H3_SEND_RETRY);
    }
  }

  return core;
}

void StopH2OCore(struct H2OCore* core)
{
  if (core != NULL)
  {
    h2o_context_dispose(&core->context);
    h2o_config_dispose(&core->global);
    uv_close((uv_handle_t*)&core->tcp, NULL);
  }
}

void ReleaseH2OCore(struct H2OCore* core)
{
  if (core != NULL)
  {
    //
    free(core);
  }
}

const char* GetH2OHeaderByIndex(const h2o_headers_t* headers, const h2o_token_t* token, size_t* size)
{
  ssize_t index;
  h2o_header_t* header;

  index = h2o_find_header(headers, token, -1);

  if (index >= 0)
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

  index = h2o_find_header_by_str(headers, name, length, -1);

  if (index >= 0)
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

  index = h2o_find_header(headers, token, -1);

  if (index >= 0)
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

  index = h2o_find_header_by_str(headers, name, length, -1);

  if (index >= 0)
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

  index = h2o_find_header(headers, token, -1);

  if (index >= 0)
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

  index = h2o_find_header_by_str(headers, name, length, -1);

  if (index >= 0)
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

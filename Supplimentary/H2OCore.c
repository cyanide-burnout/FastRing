#include "H2OCore.h"

#include <malloc.h>
#include <bsd/string.h>

static void HandleNewConnection(uv_stream_t* server, int status)
{
  uv_tcp_t* client;
  h2o_socket_t* socket;
  struct H2OCore* core;

  if (status == 0)
  {
    core   = (struct H2OCore*)server->data;
    client = (uv_tcp_t*)malloc(sizeof(uv_tcp_t));

    uv_tcp_init(core->loop, client);

    if (uv_accept(server, (uv_stream_t*)client) == 0)
    {
      socket = h2o_uv_socket_create((uv_handle_t*)client, (uv_close_cb)free);
      h2o_accept(&core->accept, socket);
      return;
    }

    uv_close((uv_handle_t*)client, (uv_close_cb)free);
  }
}

struct H2OCore* CreateH2OCore(uv_loop_t* loop, const struct sockaddr* address, SSL_CTX* context, struct H2ORoute* route, int options)
{
  static const h2o_iovec_t both[]       = { H2O_STRLIT("h2"), H2O_STRLIT("http/1.1"), { NULL, 0 } };
  static const h2o_iovec_t h2[]         = { H2O_STRLIT("h2"),                         { NULL, 0 } };
  static const h2o_iovec_t http1[]      = { H2O_STRLIT("http/1.1"),                   { NULL, 0 } };
  static const h2o_iovec_t* protocols[] = { NULL, both, h2, http1 };

  struct H2OCore* core;
  h2o_hostconf_t* host;
  h2o_pathconf_t* path;
  struct H2OHandler* handler;

  if (core = (struct H2OCore*)calloc(1, sizeof(struct H2OCore)))
  {
    uv_tcp_init(loop, &core->server);

    core->loop        = loop;
    core->server.data = core;

    if (uv_tcp_nodelay(&core->server, 1)       ||
        uv_tcp_bind(&core->server, address, 0) ||
        uv_listen((uv_stream_t*)&core->server, 128, HandleNewConnection))
    {
      uv_close((uv_handle_t*)&core->server, (uv_close_cb)free);
      return NULL;
    }

    if ((context != NULL) &&
        (options & 3))
    {
      // Set H2OCORE_ROUTE_OPTION_APLN_*
      h2o_ssl_register_alpn_protocols(context, protocols[options & 3]);
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
    core->accept.ssl_ctx = context;
  }

  return core;
}

void StopH2OCore(struct H2OCore* core)
{
  if (core != NULL)
  {
    h2o_context_dispose(&core->context);
    h2o_config_dispose(&core->global);
    uv_close((uv_handle_t*)&core->server, NULL);
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

#ifndef H2OCORE_H
#define H2OCORE_H

#include <uv.h>

#ifndef DUSE_LOCAL_H2O
#include <h2o.h>
#include <h2o/http3_server.h>
#else
#include "h2o.h"
#include "h2o/http3_server.h"
#endif

#ifdef __cplusplus
extern "C"
{
#endif

#define H2OCORE_ROUTE_CLOSURE(type, handler)  ((type)((struct H2OHandler*)handler)->closure)

#define H2OCORE_STATE_TCP_FAILED  (1 << 0)
#define H2OCORE_STATE_UDP_FAILED  (1 << 1)

#define H2OCORE_OPTION_APLN_BOTH         1
#define H2OCORE_OPTION_APLN_H2_ONLY      2
#define H2OCORE_OPTION_APLN_HTTP1_ONLY   3
#define H2OCORE_OPTION_H3_ENABLE_RETRY   (1 << 2)

#define H2OCORE_ROUTE_OPTION_STREAMING   (1 << 0)

struct H2ORoute
{
  const char *path;
  int flags;
  int options;
  int (*function)(h2o_handler_t* handler, h2o_req_t* request);
  void* closure;
};

struct H2OHandler
{
  h2o_handler_t super;
  void* closure;
};

struct H2OCore
{
  uv_tcp_t tcp;
  h2o_socket_t* udp;

  h2o_globalconf_t global;
  h2o_context_t context;
  h2o_accept_ctx_t accept;

  quicly_context_t quicly;
  ptls_aead_context_t* cache[3];
  ptls_aead_context_t* encryptor;
  ptls_aead_context_t* decryptor;
  ptls_on_client_hello_t handler;
  quicly_cid_plaintext_t identifier;
  h2o_http3_server_ctx_t server;

  uint8_t secret[PTLS_MAX_SECRET_SIZE];
  int state;
};

struct H2OCore* CreateH2OCore(uv_loop_t* loop, const struct sockaddr* address, SSL_CTX* context1, ptls_context_t* context2, struct H2ORoute* route, int options);
void StopH2OCore(struct H2OCore* core);
void ReleaseH2OCore(struct H2OCore* core);
void UpdateH2OCoreSecurity(struct H2OCore* core, SSL_CTX* context1, ptls_context_t* context2, int options);

const char* GetH2OHeaderByIndex(const h2o_headers_t* headers, const h2o_token_t* token, size_t* size);
const char* GetH2OHeaderByName(const h2o_headers_t* headers, const char* name, size_t length, size_t* size);
int CompareH2OHeaderByIndex(const h2o_headers_t* headers, const h2o_token_t* token, const void* sample, size_t size);
int CompareH2OHeaderByName(const h2o_headers_t* headers, const char* name, size_t length, const void* sample, size_t size);
int HasInH2OHeaderByIndex(const h2o_headers_t* headers, const h2o_token_t* token, const char* needle);
int HasInH2OHeaderByName(const h2o_headers_t* headers, const char* name, size_t length, const char* needle);
void MakeH2OPercentEncodedString(h2o_iovec_t* vector, h2o_mem_pool_t* pool, const char* text, size_t length);

#ifdef __cplusplus
}
#endif

#endif

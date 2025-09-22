#include <signal.h>
#include <string.h>
#include <stdio.h>

#include "FastUVLoop.h"
#include "PicoBundle.h"
#include "H2OCore.h"

atomic_int state = { 0 };

static void HandleSignal(int signal)
{
  // Interrupt main loop in case of interruption signal
  atomic_fetch_add_explicit(&state, 1, memory_order_relaxed);
}

static int HandlePageRequest(h2o_handler_t* handler, h2o_req_t* request)
{
  printf("HTTP %x request %.*s\n", request->version, (int)request->pathconf->path.len, request->pathconf->path.base);

  if (h2o_memis(request->method.base, request->method.len, H2O_STRLIT("GET")))
  {
    request->res.status = 200;
    request->res.reason = "OK";

    h2o_add_header_by_str(&request->pool, &request->res.headers, H2O_STRLIT("alt-svc"), 0, NULL, H2O_STRLIT("h3=\":8443\"; ma=86400"));
    h2o_add_header(&request->pool, &request->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("text/plain; charset=utf-8"));
    h2o_send_inline(request, H2O_STRLIT("Test\n"));

    return 0;
  }

  return -1;
}

int main()
{
  struct sigaction action;

  struct FastRing* ring;
  struct FastUVLoop* loop;
  struct H2OCore* core;

  SSL_CTX* context;
  struct PicoBundle* bundle;

  struct sockaddr_in address;

  action.sa_handler = HandleSignal;
  action.sa_flags   = SA_NODEFER | SA_RESTART;

  sigemptyset(&action.sa_mask);

  sigaction(SIGHUP,  &action, NULL);
  sigaction(SIGINT,  &action, NULL);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGQUIT, &action, NULL);

  uv_ip4_addr("0.0.0.0", 8443, &address);

  struct H2ORoute routes[] =
  {
    { "/", 0, 0, HandlePageRequest, NULL },
    { 0 }
  };

  context = SSL_CTX_new(TLS_server_method());

  SSL_CTX_set_min_proto_version(context, TLS1_3_VERSION);
  SSL_CTX_set_options(context, SSL_OP_NO_COMPRESSION | SSL_OP_IGNORE_UNEXPECTED_EOF);
  SSL_CTX_use_certificate_chain_file(context, "bundle.pem");
  SSL_CTX_use_PrivateKey_file(context, "bundle.pem", SSL_FILETYPE_PEM);

  if (SSL_CTX_check_private_key(context) != 1)
  {
    SSL_CTX_free(context);
    printf("Error loading bundle.pem\n");
    return 0;
  }

  bundle = CreatePicoBundleFromSSLContext(context);

  printf("Started\n");

  ring = CreateFastRing(0);
  loop = CreateFastUVLoop(ring, 200);
  core = CreateH2OCore(loop->loop, (struct sockaddr*)&address, context, (ptls_context_t*)bundle, routes, 0);

  while ((atomic_load_explicit(&state, memory_order_relaxed) < 1) &&
         (WaitForFastRing(ring, 200, NULL) >= 0));

  printf("Shutting down...\n");

  StopH2OCore(core);
  DepleteFastUVLoop(loop, 2000, 0, NULL, NULL);

  ReleaseH2OCore(core);
  ReleaseFastUVLoop(loop);
  ReleaseFastRing(ring);

  ReleasePicoBundle(bundle);
  SSL_CTX_free(context);

  printf("Stopped\n");

  return 0;
}

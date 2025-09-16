#ifndef PICOBUNDLE_H
#define PICOBUNDLE_H

#ifndef USE_LOCAL_PICOTLS
#include <picotls.h>
#include <picotls/openssl.h>
#include <picotls/certificate_compression.h>
#else
#include "picotls.h"
#include "picotls/openssl.h"
#include "picotls/certificate_compression.h"
#endif

#ifdef __cplusplus
extern "C"
{
#endif

struct PicoBundle
{
  ptls_context_t context;
  ptls_openssl_sign_certificate_t signer;
  ptls_emit_compressed_certificate_t emitter;
  EVP_PKEY* key;
#ifndef __cplusplus
  ptls_update_open_count_t handler;
  int _Atomic count;
#endif
};

struct PicoBundle* CreatePicoBundleFromSSLContext(SSL_CTX* context);
struct PicoBundle* AcquirePicoBundle(struct PicoBundle* bundle);
void ReleasePicoBundle(struct PicoBundle* bundle);

#ifdef __cplusplus
}
#endif

#endif

#ifndef PICOBUNDLE_H
#define PICOBUNDLE_H

#ifndef USE_LOCAL_PICOTLS
#include <picotls.h>
#include <picotls/openssl.h>
#else
#include "picotls.h"
#include "picotls/openssl.h"
#endif

#ifdef __cplusplus
extern "C"
{
#endif

struct PicoBundle
{
  ptls_context_t context;
  ptls_openssl_sign_certificate_t signer;
  EVP_PKEY* key;
};

struct PicoBundle* CreatePicoBundleFromSSLContext(SSL_CTX* context);
struct PicoBundle* CreatePicoBundleFromFile(const char* path);
void ReleasePicoBundle(struct PicoBundle* bundle);

#ifdef __cplusplus
}
#endif

#endif

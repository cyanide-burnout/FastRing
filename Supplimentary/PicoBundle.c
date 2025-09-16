#include "PicoBundle.h"

#include <malloc.h>
#include <stddef.h>
#include <stdatomic.h>
#include <openssl/ssl.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

static ptls_key_exchange_algorithm_t* exchanges[] = { &ptls_openssl_secp256r1, &ptls_openssl_x25519, NULL };

static void HandleReferenceCountUpdate(ptls_update_open_count_t* handler, ssize_t delta)
{
  struct PicoBundle* bundle;

  bundle = (struct PicoBundle*)((uint8_t*)handler - offsetof(struct PicoBundle, handler));

  atomic_fetch_add_explicit(&bundle->count, delta + 1, memory_order_relaxed);
  ReleasePicoBundle(bundle);
}

struct PicoBundle* CreatePicoBundleFromSSLContext(SSL_CTX* context)
{
  struct PicoBundle* bundle;
  STACK_OF(X509)* chain;
  X509* certificate;

  if ((context != NULL) &&
      (bundle   = (struct PicoBundle*)calloc(1, sizeof(struct PicoBundle))))
  {
    bundle->context.random_bytes  = ptls_openssl_random_bytes;
    bundle->context.get_time      = &ptls_get_time;
    bundle->context.key_exchanges = exchanges;  // ptls_openssl_key_exchanges;
    bundle->context.cipher_suites = ptls_openssl_cipher_suites;

    bundle->key = SSL_CTX_get0_privatekey(context);
    certificate = SSL_CTX_get0_certificate(context);
    chain       = NULL;

    if ((bundle->key == NULL) ||
        (EVP_PKEY_up_ref((EVP_PKEY*)bundle->key) != 1))
    {
      free(bundle);
      return NULL;
    }

    if ((certificate == NULL) ||
        (SSL_CTX_get0_chain_certs(context, &chain)           != 1) &&
        (SSL_CTX_get_extra_chain_certs_only(context, &chain) != 1) ||
        (ptls_openssl_load_certificates(&bundle->context, certificate, chain) != 0))
    {
      EVP_PKEY_free(bundle->key);
      free(bundle);
      return NULL;
    }

    ptls_init_compressed_certificate(&bundle->emitter, bundle->context.certificates.list, bundle->context.certificates.count, ptls_iovec_init(NULL, 0));
    ptls_openssl_init_sign_certificate(&bundle->signer, bundle->key);

    bundle->context.emit_certificate  = &bundle->emitter.super;
    bundle->context.sign_certificate  = &bundle->signer.super;
    bundle->context.update_open_count = &bundle->handler;
    bundle->handler.cb                = HandleReferenceCountUpdate;

    atomic_store_explicit(&bundle->count, 1, memory_order_release);

    printf("certificates %d\n", bundle->context.certificates.count);
  }

  return bundle;
}

struct PicoBundle* AcquirePicoBundle(struct PicoBundle* bundle)
{
  atomic_fetch_add_explicit(&bundle->count, 1, memory_order_relaxed);
  return bundle;
}

void ReleasePicoBundle(struct PicoBundle* bundle)
{
  if ((bundle != NULL) &&
      (atomic_fetch_sub_explicit(&bundle->count, 1, memory_order_acquire) == 1))
  {
    ptls_openssl_dispose_sign_certificate(&bundle->signer);
    ptls_dispose_compressed_certificate(&bundle->emitter);
    // FreeCertificateChain(&bundle->context);
    EVP_PKEY_free(bundle->key);
    free(bundle);
  }
}

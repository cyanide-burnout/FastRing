#include "PicoBundle.h"

#include <stdio.h>
#include <malloc.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

struct PicoBundle* CreatePicoBundleFromSSLContext(SSL_CTX* context)
{
  struct PicoBundle* bundle;

  if ((context != NULL) &&
      (bundle   = (struct PicoBundle*)calloc(1, sizeof(struct PicoBundle))))
  {
    bundle->context.random_bytes  = ptls_openssl_random_bytes;
    bundle->context.get_time      = &ptls_get_time;
    bundle->context.key_exchanges = ptls_openssl_key_exchanges;
    bundle->context.cipher_suites = ptls_openssl_cipher_suites;

    bundle->key = SSL_CTX_get0_privatekey(context);
    EVP_PKEY_up_ref((EVP_PKEY*)bundle->key);

    // TODO: copy chain

    ptls_openssl_init_sign_certificate(&bundle->signer, bundle->key);

    bundle->context.sign_certificate    = &bundle->signer.super;
    bundle->context.max_early_data_size = 0;
  }

  return bundle;

}

struct PicoBundle* CreatePicoBundleFromFile(const char* path)
{
  struct PicoBundle* bundle;
  FILE* file;

  if ((path   != NULL) &&
      (bundle  = (struct PicoBundle*)calloc(1, sizeof(struct PicoBundle))))
  {
    bundle->context.random_bytes  = ptls_openssl_random_bytes;
    bundle->context.get_time      = &ptls_get_time;
    bundle->context.key_exchanges = ptls_openssl_key_exchanges;
    bundle->context.cipher_suites = ptls_openssl_cipher_suites;

    if (!ptls_load_certificates(&bundle->context, path) ||
        !(file = fopen(path, "r")))
    {
      free(bundle);
      return NULL;
    }

    bundle->key = PEM_read_PrivateKey(file, NULL, NULL, NULL);
    fclose(file);

    ptls_openssl_init_sign_certificate(&bundle->signer, bundle->key);

    bundle->context.sign_certificate    = &bundle->signer.super;
    bundle->context.max_early_data_size = 0;
  }

  return bundle;
}


void ReleasePicoBundle(struct PicoBundle* bundle)
{
  if (bundle != NULL)
  {
    ptls_openssl_dispose_sign_certificate(&bundle->signer);
    EVP_PKEY_free(bundle->key);
    free(bundle->context.certificates.list);
    free(bundle);
  }
}

#include "AAAClient.h"

#include <alloca.h>
#include <endian.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/md5.h>

int PackRADIUSIntegerAttribute(struct RADIUSAttribute* attribute, uint8_t type, uint32_t value)
{
  attribute->type   = type;
  attribute->length = 6;
  attribute->value  = htobe32(value);
  return 6;
}

void MakeRADIUSAuthenticator(struct RADIUSDataUnit* unit, uint8_t* authenticator, const char* secret)
{
  unsigned int size;
  unsigned int length;
  EVP_MD_CTX* context;

  size    = be16toh(unit->length);
  length  = strlen(secret);
  context = EVP_MD_CTX_create();

  if (authenticator == NULL)
  {
    authenticator = (uint8_t*)alloca(MD5_DIGEST_LENGTH);
    memset(authenticator, 0, MD5_DIGEST_LENGTH);
  }

  EVP_DigestInit_ex(context, EVP_md5(), NULL);
  EVP_DigestUpdate(context, (uint8_t*)unit, offsetof(struct RADIUSDataUnit, authenticator));
  EVP_DigestUpdate(context, authenticator, MD5_DIGEST_LENGTH);
  EVP_DigestUpdate(context, unit->data, size - sizeof(struct RADIUSDataUnit));
  EVP_DigestUpdate(context, (const uint8_t*)secret, length);
  EVP_DigestFinal_ex(context, unit->authenticator, &length);
  EVP_MD_CTX_destroy(context);
}

int CheckRADIUSAuthenticator(struct RADIUSDataUnit* unit, uint8_t* authenticator, const char* secret)
{
  unsigned int size;
  unsigned int length;
  EVP_MD_CTX* context;
  uint8_t digest[MD5_DIGEST_LENGTH];

  size    = be16toh(unit->length);
  length  = strlen(secret);
  context = EVP_MD_CTX_create();

  if (authenticator == NULL)
  {
    authenticator = (uint8_t*)alloca(MD5_DIGEST_LENGTH);
    memset(authenticator, 0, MD5_DIGEST_LENGTH);
  }

  EVP_DigestInit_ex(context, EVP_md5(), NULL);
  EVP_DigestUpdate(context, (uint8_t*)unit, offsetof(struct RADIUSDataUnit, authenticator));
  EVP_DigestUpdate(context, authenticator, MD5_DIGEST_LENGTH);
  EVP_DigestUpdate(context, unit->data, size - sizeof(struct RADIUSDataUnit));
  EVP_DigestUpdate(context, (const uint8_t*)secret, length);
  EVP_DigestFinal_ex(context, digest, &length);
  EVP_MD_CTX_destroy(context);

  return memcmp(unit->authenticator, digest, MD5_DIGEST_LENGTH);
}

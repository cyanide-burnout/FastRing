# PicoBundle API Reference

Header: `Supplimentary/PicoBundle.h`

`PicoBundle` bridges OpenSSL and picotls certificate/signing context. It lets an
HTTP/3 listener reuse the certificate and private key already loaded into an
`SSL_CTX`, instead of configuring picotls separately.

Status: within this repository the module is exercised only by `Examples/H2H3Server`,
through `H2OCore`.

## API

```c
struct PicoBundle* CreatePicoBundleFromSSLContext(SSL_CTX* context);
struct PicoBundle* AcquirePicoBundle(struct PicoBundle* bundle);
void ReleasePicoBundle(struct PicoBundle* bundle);
```

- `CreatePicoBundleFromSSLContext()` derives a picotls certificate chain, an OpenSSL
  sign-certificate callback and a compressed-certificate emitter from an existing
  `SSL_CTX`.
- `ptls_context_t` is the **first member** of `struct PicoBundle`, so the bundle
  pointer is passed directly where a `ptls_context_t*` is expected — that is how it
  reaches `CreateH2OCore()` and `UpdateH2OCoreSecurity()`.
- The bundle is reference counted. `AcquirePicoBundle()` takes a reference,
  `ReleasePicoBundle()` drops one, and everything is freed at zero. The count is also
  driven by picotls itself through `ptls_context_t::update_open_count`, so a bundle
  stays alive while connections still use it. This is what makes certificate rotation
  safe: a new bundle can be installed while in-flight QUIC connections still hold the
  previous one.
- The `handler` and `count` fields are hidden from C++ translation units, so C++ sees a
  shorter `struct PicoBundle`: C++ code must not allocate, copy or embed it or rely on its
  size. `AcquirePicoBundle()` and `ReleasePicoBundle()` work from either language.
- `CreatePicoBundleFromSSLContext()` fixes the key exchanges to secp256r1 and X25519 and the
  cipher suites to those of `ptls_openssl`. It returns `NULL` for a `NULL` context, a
  context without a private key or certificate, a certificate chain that cannot be
  loaded, or a key type picotls cannot sign with.

## Lifecycle

`PicoBundle` creates nothing on the ring. The creator holds the first reference. The bundle
takes its own reference to the private key and copies the certificate chain, so the
`SSL_CTX` may be freed afterwards. picotls adds a reference for every connection created from
the context and drops it when the connection is freed; the counter is atomic.

`H2OCore` uses the bundle without a reference of its own. Keep yours while a core may start
connections with it, and drop it after the core is released or has been switched to another
bundle with `UpdateH2OCoreSecurity()`; open connections keep the old bundle alive.


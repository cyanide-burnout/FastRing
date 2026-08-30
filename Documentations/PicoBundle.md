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
- The `count` and `update_open_count` fields are hidden from C++ translation units, so
  the reference counting is only usable from C.


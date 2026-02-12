# PicoBundle API Reference

Header: `Supplimentary/PicoBundle.h`

`PicoBundle` bridges OpenSSL and picotls certificate/signing context.

## API

```c
struct PicoBundle* CreatePicoBundleFromSSLContext(SSL_CTX* context);
struct PicoBundle* AcquirePicoBundle(struct PicoBundle* bundle);
void ReleasePicoBundle(struct PicoBundle* bundle);
```


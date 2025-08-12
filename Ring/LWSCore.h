#ifndef LWSCORE_H
#define LWSCORE_H

// https://gist.github.com/iUltimateLP/17604e35f0d7a859c7a263075581f99a
// https://github.com/eclipse/mosquitto/blob/master/src/websockets.c
// https://github.com/warmcat/libwebsockets
// https://libwebsockets.org

// DEPENDENCIES += libwebsockets glib-2.0

#include <glib.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>

#ifdef USE_LOCAL_LWS
#include "libwebsockets.h"
#else
#include <libwebsockets.h>
#endif

#include "FastGLoop.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define LWS_OPTION_MINIMAL_SSL_VERSION_MASK  0x0fff
#define LWS_OPTION_IGNORE_CERTIFICATE        (1 << 12)

struct LWSSession;
struct LWSMessage;

typedef void (*LWSReportFunction)(int priority, const char* format, ...);
typedef void (*LWSCreateFunction)(SSL_CTX* context, void* closure);
typedef int  (*LWSHandleFunction)(struct LWSSession* session, enum lws_callback_reasons reason, char* data, size_t length);

struct LWSCore
{
  int option;
  void* closure;
  struct FastGLoop* loop;
  LWSCreateFunction function;

  struct lws_context* context;
  struct lws_context_creation_info information;
  struct lws_protocols protocols[2];
};

struct LWSSession
{
  void* closure;
  struct lws* instance;
  struct FastGLoop* loop;
  LWSHandleFunction function;

  struct LWSMessage* heap;
  struct LWSMessage* head;
  struct LWSMessage* tail;

  char* host;
  char* location;
  char* protocols;
  char address[INET6_ADDRSTRLEN + 1];
  struct lws_client_connect_info information;
};

struct LWSMessage
{
  struct LWSSession* session;        //
  struct LWSMessage* next;           //
  size_t size;                       // Size of allocation

  enum lws_write_protocol protocol;  // Sub-protocol (LWS_WRITE_TEXT, LWS_WRITE_BINARY, LWS_WRITE_PING, LWS_WRITE_CONTINUATION, LWS_WRITE_NO_FIN)
  size_t length;                     // Length of data
  char* data;                        // Pointer to data (will be set to buffer[LWS_PRE] by default, use NULL to close connection)

  char buffer[0];                    //
};

void SetLWSReportHandler(int level, LWSReportFunction function);   // SetLWSReportHandler(LLL_ERR | LLL_WARN, report);

struct LWSCore* CreateLWSCore(struct FastGLoop* loop, int option, int depth, LWSCreateFunction function, void* closure);  // LWS_OPTION_IGNORE_CERTIFICATE | SSL3_VERSION
void ReleaseLWSCore(struct LWSCore* core);

struct LWSSession* CreateLWSSessionFromURL(struct LWSCore* core, const char* location, const char* protocols, LWSHandleFunction function, void* closure);
struct LWSSession* CreateLWSSessionFromAddress(struct LWSCore* core, struct sockaddr* address, int secure, const char* host, const char* path, const char* protocols, LWSHandleFunction function, void* closure);
void ReleaseLWSSession(struct LWSSession* session);

struct LWSMessage* AllocateLWSMessage(struct LWSSession* session, size_t length, enum lws_write_protocol protocol);
void TransmitLWSMessage(struct LWSMessage* message);

#ifdef __cplusplus
}
#endif

#endif

#include "LWSCore.h"

#include <glib.h>
#include <malloc.h>
#include <string.h>
#include <syslog.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/ssl.h>

// Logger

static LWSReportFunction report;

static void EmitReport(int level, const char* line)
{
  switch (level)
  {
    case LLL_ERR:     report(LOG_ERR,     "LWS: %s", line);  break;
    case LLL_WARN:    report(LOG_WARNING, "LWS: %s", line);  break;
    case LLL_NOTICE:  report(LOG_NOTICE,  "LWS: %s", line);  break;
    case LLL_INFO:    report(LOG_INFO,    "LWS: %s", line);  break;
    default:          report(LOG_DEBUG,   "LWS: %s", line);  break;
  }
}

void SetLWSReportHandler(int level, LWSReportFunction function)
{
  report = function;
  lws_set_log_level(level, EmitReport);
}

// Queue

static int TransmitPendingMessages(struct lws* instance, struct LWSSession* session)
{
  int result;
  struct LWSMessage* message;

  result  = 0;
  message = NULL;

  while ((message        = session->head) &&
         (message->data != NULL)          &&
         (result = lws_write(instance, message->data, message->length, message->protocol)) &&
         (result > 0))
  {
    session->head = message->next;
    message->next = session->heap;
    session->heap = message;
  }

  if ((result         < 0)    ||
      (message       != NULL) &&
      (message->data == NULL))
  {
    // All pending messages will be released by ReleaseLWSSession()
    return -1;
  }

  if (session->head != NULL)
  {
    // Postpone next transmission
    lws_callback_on_writable(instance);
  }

  return 0;
}

struct LWSMessage* AllocateLWSMessage(struct LWSSession* session, size_t length, enum lws_write_protocol protocol)
{
  size_t size;
  struct LWSMessage* message;

  size = sizeof(struct LWSMessage) + LWS_PRE + length;

  if (session->heap != NULL)
  {
    message       = session->heap;
    session->heap = message->next;

    if (size <= message->size)
    {
      message->next     = NULL;
      message->data     = message->buffer + LWS_PRE;
      message->length   = length;
      message->protocol = protocol;
      return message;
    }

    free(message);
  }

  if (message = (struct LWSMessage*)malloc(size))
  {
    message->session  = session;
    message->size     = size;
    message->next     = NULL;
    message->data     = message->buffer + LWS_PRE;
    message->length   = length;
    message->protocol = protocol;
  }

  return message;
}

void TransmitLWSMessage(struct LWSMessage* message)
{
  struct LWSSession* session;

  session       = message->session;
  message->next = NULL;

  if (session->head != NULL)
  {
    session->tail->next = message;
    session->tail       = message;
    return;
  }

  session->head = message;
  session->tail = message;
  lws_callback_on_writable(session->instance);
  TouchFastGLoop(session->loop);
}

// Core

static int HandleCertificate(X509_STORE_CTX* context, void* argument)
{
  return 1;
}

static int HandleServiceEvent(struct lws* instance, enum lws_callback_reasons reason, void* user, void* data, size_t length)
{
  SSL_CTX* context;
  struct LWSCore* core;
  struct LWSSession* session;

  switch (reason)
  {
    case LWS_CALLBACK_WSI_DESTROY:
      session = (struct LWSSession*)user;
      if (session != NULL)
      {
        // Instance is already destroyed and pointer has to be cleared before calling a function
        session->instance = NULL;
        return session->function(session, reason, NULL, 0);
      }

    case LWS_CALLBACK_PROTOCOL_INIT:
    case LWS_CALLBACK_PROTOCOL_DESTROY:
      return 0;

    case LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS:
      context = (SSL_CTX*)user;
      core    = (struct LWSCore*)lws_context_user(lws_get_context(instance));
      if (core->option & LWS_OPTION_MINIMAL_SSL_VERSION_MASK)  SSL_CTX_set_min_proto_version(context, core->option & LWS_OPTION_MINIMAL_SSL_VERSION_MASK);
      if (core->option & LWS_OPTION_IGNORE_CERTIFICATE)        SSL_CTX_set_cert_verify_callback(context, HandleCertificate, NULL);
      if (core->function != NULL)                              core->function(context, core->closure);
      return 0;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
      session = (struct LWSSession*)user;
      if ((session       != NULL) &&
          (session->head != NULL))
      {
        // Instance is ready to send, queue mode is activated
        return TransmitPendingMessages(instance, session);
      }

    default:
      session = (struct LWSSession*)user;
      if (session != NULL)
      {
        // All following events can be handled on application layer
        return session->function(session, reason, (char*)data, length);
      }
  }

  return -1;
}

struct LWSCore* CreateLWSCore(struct FastGLoop* loop, int option, int depth, LWSCreateFunction function, void* closure)
{
  struct LWSCore* core;
  struct lws_protocols* protocol;

  core = (struct LWSCore*)calloc(sizeof(struct LWSCore), 1);

  core->protocols[0].name     = "default";
  core->protocols[0].callback = HandleServiceEvent;
  core->protocols[0].user     = core;

  core->information.port          = CONTEXT_PORT_NO_LISTEN;
  core->information.options       = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT | LWS_SERVER_OPTION_SSL_ECDH | LWS_SERVER_OPTION_GLIB;
  core->information.protocols     = core->protocols;
  core->information.gid           = -1;
  core->information.uid           = -1;
  core->information.user          = core;
  core->information.foreign_loops = (void**)&loop->loop;
  core->information.count_threads = 1;

#if (LWS_LIBRARY_VERSION_MAJOR > 4) || (LWS_LIBRARY_VERSION_MAJOR == 4) && (LWS_LIBRARY_VERSION_MINOR >= 2)
   core->information.smd_queue_depth = depth;
#endif

  core->loop     = loop;
  core->option   = option;
  core->closure  = closure;
  core->function = function;
  core->context  = lws_create_context(&core->information);

  return core;
}

void ReleaseLWSCore(struct LWSCore* core)
{
#if (LWS_LIBRARY_VERSION_MAJOR < 4) || (LWS_LIBRARY_VERSION_MAJOR == 4) && (LWS_LIBRARY_VERSION_MINOR < 3)
  lws_context_destroy2(core->context);
#else
  lws_context_destroy(core->context);
#endif
  free(core);
}

struct LWSSession* CreateLWSSessionFromURL(struct LWSCore* core, const char* location, const char* protocols, LWSHandleFunction function, void* closure)
{
  struct LWSSession* session;
  const char* scheme;
  int position;
  int length;

  length  = strlen(location);
  session = (struct LWSSession*)calloc(1, sizeof(struct LWSSession));

  session->location = (char*)calloc(length + 2, 1);
  strcpy(session->location, location);

  if (lws_parse_uri(session->location, &scheme, &session->information.host, &session->information.port, &session->information.path) != 0)
  {
    free(session->location);
    free(session);
    return NULL;
  }

  if (session->information.path[0] != '/')
  {
    position  = session->information.path - session->location;
    length   -= position;
    memmove(session->location + position + 1, session->location + position, length);
    session->location[position] = '/';
  }

  if (protocols != NULL)
  {
    // List of protocols has been passed
    session->protocols = strdup(protocols);
  }

  session->function  = function;
  session->closure   = closure;
  session->loop      = core->loop;

  session->information.context                   = core->context;
  session->information.address                   = session->information.host;
  session->information.ssl_connection            = (strcmp(scheme, "wss") == 0) || (strcmp(scheme, "https") == 0);
  session->information.ietf_version_or_minus_one = -1;
  session->information.protocol                  = session->protocols;
  session->information.userdata                  = session;

  session->instance = lws_client_connect_via_info(&session->information);

  TouchFastGLoop(core->loop);

  return session;
}

struct LWSSession* CreateLWSSessionFromAddress(struct LWSCore* core, struct sockaddr* address, int secure, const char* host, const char* path, const char* protocols, LWSHandleFunction function, void* closure)
{
  struct LWSSession* session;
  struct sockaddr_in6* v6;
  struct sockaddr_in* v4;
  int condition;

  session = (struct LWSSession*)calloc(1, sizeof(struct LWSSession));

  switch (address->sa_family)
  {
    case AF_INET:
      v4 = (struct sockaddr_in*)address;
      session->information.port = ntohs(v4->sin_port);
      inet_ntop(AF_INET, &v4->sin_addr, session->address, INET6_ADDRSTRLEN + 1);
      break;

    case AF_INET6:
      v6 = (struct sockaddr_in6*)address;
      session->information.port = ntohs(v6->sin6_port);
      condition = IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr);
      inet_ntop(AF_INET6 - 8 * condition, v6->sin6_addr.s6_addr + 12 * condition, session->address, INET6_ADDRSTRLEN + 1);
      break;

    default:
      free(session);
      return NULL;
  }

  session->information.address = session->address;
  session->information.host    = session->address;

  if (host != NULL)
  {
    session->host             = strdup(host);
    session->information.host = session->host;
  }

  if (protocols != NULL)
  {
    // List of protocols has been passed
    session->protocols = strdup(protocols);
  }

  session->location  = strdup(path);
  session->function  = function;
  session->closure   = closure;
  session->loop      = core->loop;

  session->information.context                   = core->context;
  session->information.ssl_connection            = secure;
  session->information.path                      = session->location;
  session->information.ietf_version_or_minus_one = -1;
  session->information.protocol                  = session->protocols;
  session->information.userdata                  = session;

  session->instance = lws_client_connect_via_info(&session->information);

  TouchFastGLoop(core->loop);

  return session;
}

void ReleaseLWSSession(struct LWSSession* session)
{
  struct LWSMessage* message;

  if (session->instance != NULL)
  {
    lws_close_reason(session->instance, LWS_CLOSE_STATUS_NOSTATUS, NULL, 0);
    lws_set_wsi_user(session->instance, NULL);
  }

  while (message = session->heap)
  {
    session->heap = message->next;
    free(message);
  }

  while (message = session->head)
  {
    session->head = message->next;
    free(message);
  }

  free(session->protocols);
  free(session->location);
  free(session->host);
  free(session);
}

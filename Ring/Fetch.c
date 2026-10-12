#define _GNU_SOURCE

#include "Fetch.h"

#include <malloc.h>
#include <unistd.h>
#include <string.h>
#include <syslog.h>
#include <stdatomic.h>

#include <netdb.h>
#include <stdio.h>
#include <stdarg.h>
#include <alloca.h>
#include <arpa/inet.h>

#define ALLOCATION_SIZE  16384

// A handler is free to close its own transmission, but ReleaseFetchTransmission() calls
// that handler and then frees the object, so the nested call has to be ignored
#define FETCH_CONDITION_RELEASE  (1 << 0)

_Static_assert((ALLOCATION_SIZE & (ALLOCATION_SIZE - 1)) == 0, "ALLOCATION_SIZE must be power of two");

static void HandleSocketEvent(struct FastRingDescriptor* descriptor, int result);
static void HandleTimeoutEvent(struct FastRingDescriptor* descriptor);
static void HandleFlushEvent(void* closure, int reason);

static void TouchTransmissionQueue(struct Fetch* fetch)
{
  if (fetch->flusher == NULL)
  {
    // HandleFlushEvent should be only set once per cycle
    fetch->flusher = SetFastRingFlushHandler(fetch->ring, HandleFlushEvent, fetch);
  }
}

static void HandleTimeoutEvent(struct FastRingDescriptor* descriptor)
{
  struct Fetch* fetch;
  int count;

  fetch             = (struct Fetch*)descriptor->closure;
  fetch->descriptor = NULL;

  curl_multi_socket_action(fetch->multi, CURL_SOCKET_TIMEOUT, 0, &count);
  TouchTransmissionQueue(fetch);
}

static void HandleSocketEvent(struct FastRingDescriptor* descriptor, int result)
{
  struct Fetch* fetch;
  int count;
  int mask;

  fetch = (struct Fetch*)descriptor->closure;

  // A failed poll, on a closed descriptor for example, is reported as an error: the watch would repeat it forever
  mask = CURL_CSELECT_IN | CURL_CSELECT_ERR;

  if (result > 0)
  {
    mask =
      (((result & (POLLIN                       )) != 0) * (CURL_CSELECT_IN                    )) |
      (((result & (POLLOUT                      )) != 0) * (CURL_CSELECT_OUT                   )) |
      (((result & (POLLERR | POLLHUP | POLLRDHUP)) != 0) * (CURL_CSELECT_IN  | CURL_CSELECT_ERR));
  }

  curl_multi_socket_action(fetch->multi, descriptor->data.watch.handle, mask, &count);
  TouchTransmissionQueue(fetch);
}

static int HandleSocketOperation(CURL* easy, curl_socket_t handle, int operation, void* data1, void* data2)
{
  struct Fetch* fetch;
  struct FastRingDescriptor* descriptor;
  uint32_t mask;

  fetch      = (struct Fetch*)data1;
  descriptor = (struct FastRingDescriptor*)data2;

  switch (operation)
  {
    case CURL_POLL_IN:     mask = POLLIN  | POLLHUP | POLLRDHUP | POLLERR;              break;
    case CURL_POLL_OUT:    mask = POLLOUT | POLLHUP | POLLRDHUP | POLLERR;              break;
    case CURL_POLL_INOUT:  mask = POLLIN  | POLLOUT | POLLHUP   | POLLRDHUP | POLLERR;  break;
    default:               mask = 0;                                                    break;
  }

  if (mask == 0)
  {
    // Socket is gone, also when it is removed from inside its own event
    curl_multi_assign(fetch->multi, handle, NULL);
    RemoveFastRingWatch(descriptor);
    return 0;
  }

  if (descriptor != NULL)
  {
    // Watch API takes care of a poll already in the kernel or of a call from its own event
    UpdateFastRingWatch(descriptor, mask);
    return 0;
  }

  if (descriptor = AddFastRingWatch(fetch->ring, handle, mask, 0, HandleSocketEvent, fetch))
  {
    curl_multi_assign(fetch->multi, handle, descriptor);
    return 0;
  }

  return -1;
}

static int HandleTimerOperation(CURLM* multi, long timeout, void* data)
{
  struct Fetch* fetch;

  fetch             = (struct Fetch*)data;
  fetch->descriptor = SetFastRingTimeout(fetch->ring, fetch->descriptor, timeout, 0, HandleTimeoutEvent, fetch);

  return 0;
}

static size_t HandleWrite(void* buffer, size_t size, size_t count, void* data)
{
  struct FetchTransmission* transmission;
  curl_off_t capacity;
  size_t length;
  char* pointer;

  transmission  = (struct FetchTransmission*)data;
  size         *= count;
  length        = transmission->length + size;

  if (transmission->buffer == NULL)
  {
    curl_easy_getinfo(transmission->easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &capacity);
    if (capacity > 0)
    {
      pointer                = NULL;
      transmission->capacity = capacity + 1;
      transmission->buffer   = (char*)malloc(transmission->capacity);
    }
  }

  if (length >= transmission->capacity)
  {
    pointer                = transmission->buffer;
    transmission->capacity = (length + ALLOCATION_SIZE) & ~(ALLOCATION_SIZE - 1);
    transmission->buffer   = (char*)realloc(transmission->buffer, transmission->capacity);
  }

  if ((transmission->buffer   == NULL) &&
      (transmission->capacity  > 0))
  {
    free(pointer);
    return 0;
  }

  if ((size                  > 0) &&
      (transmission->buffer != NULL))
  {
    pointer              = transmission->buffer + transmission->length;
    transmission->length = length;
    pointer[size]        = '\0';
    memcpy(pointer, buffer, size);
  }

  return size;
}

static void ReleaseFetchTransmission(struct FetchTransmission* transmission)
{
  struct Fetch* fetch;
  char* error;
  long code;

  if (~transmission->condition & FETCH_CONDITION_RELEASE)
  {
    transmission->condition |= FETCH_CONDITION_RELEASE;

    fetch = transmission->fetch;
    code  = 0;

    if (transmission->function != NULL)
    {
      switch (transmission->state)
      {
        case CURLE_OK:
          curl_easy_getinfo(transmission->easy, CURLINFO_RESPONSE_CODE, &code);
          transmission->function(transmission, transmission->easy, code, transmission->buffer, transmission->length, transmission->parameter1, transmission->parameter2);
          break;

        case FETCH_STATUS_INCOMPLETE:
        case FETCH_STATUS_CANCELLED:
          transmission->function(transmission, transmission->easy, transmission->state, NULL, 0, transmission->parameter1, transmission->parameter2);
          break;

        default:
          error = (char*)curl_easy_strerror(transmission->state);
          transmission->function(transmission, transmission->easy, -transmission->state, error, 0, transmission->parameter1, transmission->parameter2);
          break;
      }
    }

    curl_multi_remove_handle(fetch->multi, transmission->easy);
    curl_easy_cleanup(transmission->easy);
    free(transmission->buffer);
    free(transmission);
  }
}

static void ProceedTransmissionQueue(struct Fetch* fetch)
{
  int count;
  struct CURLMsg* message;
  struct FetchTransmission* transmission;

  count = 0;

  while (message = curl_multi_info_read(fetch->multi, &count))
  {
    if (message->msg == CURLMSG_DONE)
    {
      curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, (char**)&transmission);

      if ((message->data.result != CURLE_OK) ||
          (transmission->state  == FETCH_STATUS_INCOMPLETE))
      {
        //
        transmission->state = message->data.result;
      }

      ReleaseFetchTransmission(transmission);
    }
  }
}

static void HandleFlushEvent(void* closure, int reason)
{
  struct Fetch* fetch;

  if (reason == RING_REASON_COMPLETE)
  {
    fetch          = (struct Fetch*)closure;
    fetch->flusher = NULL;

    ProceedTransmissionQueue(fetch);
  }
}

struct Fetch* CreateFetch(struct FastRing* ring)
{
  struct Fetch* fetch;

  if (fetch = (struct Fetch*)calloc(1, sizeof(struct Fetch)))
  {
    fetch->ring  = ring;
    fetch->multi = curl_multi_init();
    fetch->share = curl_share_init();

    curl_multi_setopt(fetch->multi, CURLMOPT_TIMERDATA, fetch);
    curl_multi_setopt(fetch->multi, CURLMOPT_SOCKETDATA, fetch);
    curl_multi_setopt(fetch->multi, CURLMOPT_TIMERFUNCTION, HandleTimerOperation);
    curl_multi_setopt(fetch->multi, CURLMOPT_SOCKETFUNCTION, HandleSocketOperation);
    curl_multi_setopt(fetch->multi, CURLMOPT_PIPELINING, CURLPIPE_HTTP1 | CURLPIPE_MULTIPLEX);
    curl_share_setopt(fetch->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE);
    curl_share_setopt(fetch->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_HSTS);
  }

  return fetch;
}

void ReleaseFetch(struct Fetch* fetch)
{
  struct FetchTransmission* transmission;
  CURL** handle;
  CURL** list;

  // curl_multi_remove_handle() below recalculates the timeout and re-enters
  // HandleTimerOperation(), so the field must not be left pointing to a removed descriptor
  RemoveFastRingFlushHandler(fetch->ring, fetch->flusher);
  fetch->descriptor = SetFastRingTimeout(fetch->ring, fetch->descriptor, -1, 0, NULL, NULL);

  if (list = curl_multi_get_handles(fetch->multi))
  {
    handle = list;

    while (*handle != NULL)
    {
      curl_easy_getinfo(*handle, CURLINFO_PRIVATE, (char**)&transmission);
      ReleaseFetchTransmission(transmission);
      handle ++;
    }

    curl_free(list);
  }

  curl_multi_cleanup(fetch->multi);
  curl_share_cleanup(fetch->share);
  free(fetch);
}

int GetFetchTransmissionCount(struct Fetch* fetch)
{
  CURL** handle;
  CURL** list;
  int count;

  count = 0;

  if ((fetch != 0) &&
      (list   = curl_multi_get_handles(fetch->multi)))
  {
    handle = list;

    while (*handle != NULL)
    {
      count  ++;
      handle ++;
    }

    curl_free(list);
  }

  return count;
}

struct FetchTransmission* MakeExtendedFetchTransmission(struct Fetch* fetch, struct FetchTransmission* transmission, CURL* easy, int option, HandleFetchFunction function, void* parameter1, void* parameter2)
{
  int count;

  if ((easy != NULL) &&
      ((transmission != NULL) ||
       (transmission  = (struct FetchTransmission*)calloc(1, sizeof(struct FetchTransmission)))))
  {
    // A transmission provided by an application is only required to be an allocation,
    // so the accumulation buffer has to be initialized here as well
    transmission->fetch      = fetch;
    transmission->easy       = easy;
    transmission->state      = FETCH_STATUS_INCOMPLETE;
    transmission->condition  = 0;
    transmission->option     = option;
    transmission->function   = function;
    transmission->parameter1 = parameter1;
    transmission->parameter2 = parameter2;
    transmission->buffer     = NULL;
    transmission->length     = 0;
    transmission->capacity   = 0;

    curl_easy_setopt(easy, CURLOPT_SHARE, fetch->share);
    curl_easy_setopt(easy, CURLOPT_PRIVATE, transmission);
    curl_easy_setopt(easy, CURLOPT_DNS_CACHE_TIMEOUT, 600);
    curl_easy_setopt(easy, CURLOPT_ALTSVC, "");

    if (option & FETCH_OPTION_HANDLE_CONTENT)
    {
      curl_easy_setopt(easy, CURLOPT_WRITEDATA, transmission);
      curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, HandleWrite);
    }

    if (option & FETCH_OPTION_SET_HANDLER_DATA)
    {
      curl_easy_setopt(easy, CURLOPT_READDATA, transmission);
      curl_easy_setopt(easy, CURLOPT_WRITEDATA, transmission);
      curl_easy_setopt(easy, CURLOPT_HEADERDATA, transmission);
    }

    curl_multi_add_handle(fetch->multi, transmission->easy);
    curl_multi_socket_action(fetch->multi, CURL_SOCKET_TIMEOUT, 0, &count);

    // The transfer may have ended already, a failed connect for example, and nothing else would report it
    TouchTransmissionQueue(fetch);

    return transmission;
  }

  curl_easy_cleanup(easy);
  return NULL;
}

struct FetchTransmission* MakeSimpleFetchTransmission(struct Fetch* fetch, const char* location, struct curl_slist* headers, const char* token, const char* data, size_t length, HandleFetchFunction function, void* parameter1, void* parameter2)
{
  CURL* easy;

  if (easy = curl_easy_init())
  {
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(easy, CURLOPT_URL, location);

    if (headers != NULL)
    {
      // Headers could be overwritten by application
      curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    }

    if (token != NULL)
    {
      curl_easy_setopt(easy, CURLOPT_HTTPAUTH, CURLAUTH_BEARER);
      curl_easy_setopt(easy, CURLOPT_XOAUTH2_BEARER, token);
    }

    if (data != NULL)
    {
      curl_easy_setopt(easy, CURLOPT_POST, 1);
      curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, length);
      curl_easy_setopt(easy, CURLOPT_COPYPOSTFIELDS, data);
    }

    return MakeExtendedFetchTransmission(fetch, NULL, easy, FETCH_OPTION_HANDLE_CONTENT, function, parameter1, parameter2);
  }

  return NULL;
}

void CancelFetchTransmission(struct FetchTransmission* transmission)
{
  if (transmission != NULL)
  {
    transmission->state = FETCH_STATUS_CANCELLED;
    ReleaseFetchTransmission(transmission);
  }
}

void TouchFetchTransmission(struct FetchTransmission* transmission)
{
  struct Fetch* fetch;
  int count;

  fetch = transmission->fetch;

  curl_multi_socket_action(fetch->multi, CURL_SOCKET_TIMEOUT, 0, &count);
  TouchTransmissionQueue(fetch);
}

int AppendFetchParameter(CURLU* location, int size, const char* format, ...)
{
  va_list arguments;
  char* buffer;

  buffer = (char*)alloca(size);

  va_start(arguments, format);
  vsnprintf(buffer, size, format, arguments);
  va_end(arguments);

  return curl_url_set(location, CURLUPART_QUERY, buffer, CURLU_APPENDQUERY | CURLU_URLENCODE);
}

struct curl_slist* AppendFetchList(struct curl_slist* list, int size, const char* format, ...)
{
  va_list arguments;
  char* buffer;

  buffer = (char*)alloca(size);

  va_start(arguments, format);
  vsnprintf(buffer, size, format, arguments);
  va_end(arguments);

  return curl_slist_append(list, buffer);
}

struct curl_slist* MakeFetchConnectAddress(const struct sockaddr* address)
{
  const struct sockaddr_in6* version6;
  const struct sockaddr_in* version4;
  char connection[INET6_ADDRSTRLEN + 16];
  char name[INET6_ADDRSTRLEN + 1];
  int port;

  switch (address->sa_family)
  {
    case AF_INET:
      version4 = (const struct sockaddr_in*)address;
      port     = ntohs(version4->sin_port);
      inet_ntop(AF_INET, &version4->sin_addr, name, INET_ADDRSTRLEN + 1);
      sprintf(connection, "::%s:%d", name, port);
      return curl_slist_append(NULL, connection);

    case AF_INET6:
      version6 = (const struct sockaddr_in6*)address;
      port     = ntohs(version6->sin6_port);
      inet_ntop(AF_INET6, &version6->sin6_addr, name, INET6_ADDRSTRLEN + 1);
      sprintf(connection, "::[%s]:%d", name, port);
      return curl_slist_append(NULL, connection);
  }

  return NULL;
}

int HandleFetchDebug(CURL* easy, curl_infotype type, char* data, size_t size, void* closure)
{
  typedef void (*ReportFunction)(int priority, const char* format, ...);

  ReportFunction report;

  report = (ReportFunction)closure;

  switch (type)
  {
    case CURLINFO_TEXT:        report(LOG_DEBUG, "Fetch: * %.*s", (int)size, data);             break;
    case CURLINFO_HEADER_OUT:  report(LOG_DEBUG, "Fetch: => Headers: %.*s", (int)size, data);   break;
    case CURLINFO_HEADER_IN:   report(LOG_DEBUG, "Fetch: <= Headers: %.*s", (int)size, data);   break;
    case CURLINFO_DATA_OUT:    report(LOG_DEBUG, "Fetch: => Data: %zu bytes\n", size);          break;
    case CURLINFO_DATA_IN:     report(LOG_DEBUG, "Fetch: <= Data: %zu bytes\n", size);          break;
  }

  return 0;
}

#define _GNU_SOURCE

#include "Fetch.h"

#include <malloc.h>
#include <unistd.h>
#include <string.h>
#include <stdatomic.h>

#include <netdb.h>
#include <stdio.h>
#include <stdarg.h>
#include <alloca.h>
#include <arpa/inet.h>

#define ALLOCATION_SIZE  16384

#define APPEND(list, record)                   \
  record->previous = list->last;               \
  record->next = NULL;                         \
  if (list->last != NULL)                      \
    list->last->next = record;                 \
  else                                         \
    list->first = record;                      \
  list->last = record;

#define REMOVE(list, record)                   \
  if (record->previous != NULL)                \
    record->previous->next = record->next;     \
  else                                         \
    list->first = record->next;                \
  if (record->next != NULL)                    \
    record->next->previous = record->previous; \
  else                                         \
    list->last = record->previous;

_Static_assert((ALLOCATION_SIZE & (ALLOCATION_SIZE - 1)) == 0, "ALLOCATION_SIZE must be power of two");

struct ContentStorage
{
  char* buffer;
  size_t length;
  size_t capacity;
};

struct Fetch
{
  CURLM* multi;
  CURLSH* share;
  struct FastRing* ring;
  struct FastRingFlusher* flusher;
  struct FastRingDescriptor* descriptor;
  atomic_int count;

#if (LIBCURL_VERSION_NUM < 0x080400)
  struct FetchTransmission* first;
  struct FetchTransmission* last;
#endif
};

struct FetchTransmission
{
  struct Fetch* fetch;

  CURL* easy;
  int state;

  HandleFetchFunction function;
  void* parameter1;
  void* parameter2;
  int option;

#if (LIBCURL_VERSION_NUM < 0x080400)
  struct FetchTransmission* previous;
  struct FetchTransmission* next;
#endif

  union
  {
    struct ContentStorage storage;
    uint8_t data[FETCH_STORAGE_SIZE];
  };
};

static int HandleSocketCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason);
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

static int HandleSocketCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  struct Fetch* fetch;
  int count;
  int mask;

  if ((reason == RING_REASON_COMPLETE) &&
      (fetch   = (struct Fetch*)descriptor->closure))
  {
    if (completion->res > 0)
    {
      mask =
        (((completion->res & (POLLIN                       )) != 0) * (CURL_CSELECT_IN                    )) |
        (((completion->res & (POLLOUT                      )) != 0) * (CURL_CSELECT_OUT                   )) |
        (((completion->res & (POLLERR | POLLHUP | POLLRDHUP)) != 0) * (CURL_CSELECT_IN  | CURL_CSELECT_ERR));

      descriptor->data.number = CURL_CSELECT_IN;

      curl_multi_socket_action(fetch->multi, descriptor->submission.fd, mask, &count);
      TouchTransmissionQueue(fetch);

      descriptor->data.number = 0;
    }

    if (descriptor->closure != NULL)
    {
      SubmitFastRingDescriptor(descriptor, 0);
      return 1;
    }
  }

  return 0;
}

static int HandleSocketOperation(CURL* easy, curl_socket_t handle, int operation, void* data1, void* data2)
{
  struct Fetch* fetch;
  struct FastRingDescriptor* descriptor;

  fetch      = (struct Fetch*)data1;
  descriptor = (struct FastRingDescriptor*)data2;

  if (descriptor != NULL)
  {
    if (operation == CURL_POLL_REMOVE)
    {
      curl_multi_assign(fetch->multi, handle, NULL);
      descriptor->function = NULL;
      descriptor->closure  = NULL;
    }

    if ((descriptor->data.number == CURL_CSELECT_IN) ||
        (descriptor->state       == RING_DESC_STATE_PENDING))
    {
      switch (operation)
      {
        case CURL_POLL_IN:      descriptor->submission.poll32_events = __io_uring_prep_poll_mask(POLLIN  | POLLHUP | POLLRDHUP | POLLERR);              break;
        case CURL_POLL_OUT:     descriptor->submission.poll32_events = __io_uring_prep_poll_mask(POLLOUT | POLLHUP | POLLRDHUP | POLLERR);              break;
        case CURL_POLL_INOUT:   descriptor->submission.poll32_events = __io_uring_prep_poll_mask(POLLIN  | POLLOUT | POLLHUP   | POLLRDHUP | POLLERR);  break;
        case CURL_POLL_REMOVE:  descriptor->submission.opcode        = IORING_OP_NOP;                                                                   break;
      }

      return 0;
    }

    if (operation == CURL_POLL_REMOVE)
    {
      atomic_fetch_add_explicit(&descriptor->references, 1, memory_order_relaxed);
      io_uring_initialize_sqe(&descriptor->submission);
      io_uring_prep_poll_remove(&descriptor->submission, descriptor->identifier);
      SubmitFastRingDescriptor(descriptor, RING_DESC_OPTION_IGNORE);
      return 0;
    }
  }

  if ((operation  != CURL_POLL_REMOVE) &&
      (descriptor  = AllocateFastRingDescriptor(fetch->ring, HandleSocketCompletion, fetch)))
  {
    switch (operation)
    {
      case CURL_POLL_IN:     io_uring_prep_poll_add(&descriptor->submission, handle, POLLIN  | POLLHUP | POLLRDHUP | POLLERR);              break;
      case CURL_POLL_OUT:    io_uring_prep_poll_add(&descriptor->submission, handle, POLLOUT | POLLHUP | POLLRDHUP | POLLERR);              break;
      case CURL_POLL_INOUT:  io_uring_prep_poll_add(&descriptor->submission, handle, POLLIN  | POLLOUT | POLLHUP   | POLLRDHUP | POLLERR);  break;
    }

    curl_multi_assign(fetch->multi, handle, descriptor);
    SubmitFastRingDescriptor(descriptor, 0);
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
  struct ContentStorage* storage;
  curl_off_t capacity;
  size_t length;
  char* pointer;

  transmission  = (struct FetchTransmission*)data;
  storage       = &transmission->storage;
  size         *= count;
  length        = storage->length + size;

  if (storage->buffer == NULL)
  {
    curl_easy_getinfo(transmission->easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &capacity);
    if (capacity > 0)
    {
      pointer           = NULL;
      storage->capacity = capacity + 1;
      storage->buffer   = (char*)malloc(storage->capacity);
    }
  }

  if (length > storage->capacity)
  {
    pointer           = storage->buffer;
    storage->capacity = (length + ALLOCATION_SIZE) & ~(ALLOCATION_SIZE - 1);
    storage->buffer   = (char*)realloc(storage->buffer, storage->capacity);
  }

  if ((storage->buffer   == NULL) &&
      (storage->capacity  > 0))
  {
    free(pointer);
    return 0;
  }

  if ((size             > 0) &&
      (storage->buffer != NULL))
  {
    pointer         = storage->buffer + storage->length;
    storage->length = length;
    pointer[size]   = '\0';
    memcpy(pointer, buffer, size);
  }

  return size;
}

static void ReleaseFetchTransmission(struct FetchTransmission* transmission)
{
  struct Fetch* fetch;
  char* error;
  long code;

  fetch = transmission->fetch;
  code  = 0;

  if (transmission->function != NULL)
  {
    switch (transmission->state)
    {
      case CURLE_OK:
        curl_easy_getinfo(transmission->easy, CURLINFO_RESPONSE_CODE, &code);
        transmission->function(transmission, transmission->easy, code, transmission->storage.buffer, transmission->storage.length, transmission->parameter1, transmission->parameter2);
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

  if (transmission->option & FETCH_OPTION_HANDLE_CONTENT)
  {
    // Storage area could be used for another purposes
    free(transmission->storage.buffer);
  }

#if (LIBCURL_VERSION_NUM < 0x080400)
  REMOVE(fetch, transmission);
#endif

  atomic_fetch_sub_explicit(&fetch->count, 1, memory_order_relaxed);
  curl_multi_remove_handle(fetch->multi, transmission->easy);
  curl_easy_cleanup(transmission->easy);
  free(transmission);
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

  fetch        = (struct Fetch*)calloc(1, sizeof(struct Fetch));
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

  return fetch;
}

void ReleaseFetch(struct Fetch* fetch)
{
  struct FetchTransmission* transmission;
  CURL** handle;
  CURL** list;

  RemoveFastRingFlushHandler(fetch->ring, fetch->flusher);
  SetFastRingTimeout(fetch->ring, fetch->descriptor, -1, 0, NULL, NULL);

#if (LIBCURL_VERSION_NUM >= 0x080400)
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
#else
  while (fetch->first != NULL)
  {
    // libcurl doesn't release contexts automatically :/
    ReleaseFetchTransmission(fetch->last);
  }
#endif

  curl_multi_cleanup(fetch->multi);
  curl_share_cleanup(fetch->share);
  free(fetch);
}

struct FetchTransmission* MakeExtendedFetchTransmission(struct Fetch* fetch, CURL* easy, int option, HandleFetchFunction function, void* parameter1, void* parameter2)
{
  struct FetchTransmission* transmission;
  int count;

  transmission             = (struct FetchTransmission*)calloc(1, sizeof(struct FetchTransmission));
  transmission->fetch      = fetch;
  transmission->easy       = easy;
  transmission->state      = FETCH_STATUS_INCOMPLETE;
  transmission->option     = option;
  transmission->function   = function;
  transmission->parameter1 = parameter1;
  transmission->parameter2 = parameter2;

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

#if (LIBCURL_VERSION_NUM < 0x080400)
  APPEND(fetch, transmission);
#endif

  atomic_fetch_add_explicit(&fetch->count, 1, memory_order_relaxed);
  curl_multi_add_handle(fetch->multi, transmission->easy);
  curl_multi_socket_action(fetch->multi, CURL_SOCKET_TIMEOUT, 0, &count);

  return transmission;
}

struct FetchTransmission* MakeSimpleFetchTransmission(struct Fetch* fetch, const char* location, struct curl_slist* headers, const char* token, const char* data, size_t length, HandleFetchFunction function, void* parameter1, void* parameter2)
{
  CURL* easy;

  easy = curl_easy_init();

  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0);
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

  return MakeExtendedFetchTransmission(fetch, easy, FETCH_OPTION_HANDLE_CONTENT, function, parameter1, parameter2);
}

struct FastRing* GetFetchTransmissionRing(struct FetchTransmission* transmission)
{
  return transmission->fetch->ring;
}

void* GetFetchTransmissionParameter1(struct FetchTransmission* transmission)
{
  return transmission->parameter1;
}

void* GetFetchTransmissionParameter2(struct FetchTransmission* transmission)
{
  return transmission->parameter2;
}

void* GetFetchTransmissionStorage(struct FetchTransmission* transmission)
{
  return transmission->data;
}

CURL* GetFetchTransmissionHandle(struct FetchTransmission* transmission)
{
  return transmission->easy;  
}

void TouchFetchTransmission(struct FetchTransmission* transmission)
{
  struct Fetch* fetch;
  int count;

  fetch = transmission->fetch;

  curl_multi_socket_action(fetch->multi, CURL_SOCKET_TIMEOUT, 0, &count);
  TouchTransmissionQueue(fetch);
}

void CancelFetchTransmission(struct FetchTransmission* transmission)
{
  if (transmission != NULL)
  {
    transmission->state = FETCH_STATUS_CANCELLED;
    ReleaseFetchTransmission(transmission);
  }
}

int GetFetchTransmissionCount(struct Fetch* fetch)
{
  return atomic_load_explicit(&fetch->count, memory_order_relaxed);
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

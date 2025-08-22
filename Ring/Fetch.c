#define _GNU_SOURCE
#define Fetch              struct FetchContext
#define FetchTransmission  struct TransmissionContext

#include "Fetch.h"

#include <malloc.h>
#include <unistd.h>
#include <string.h>
#include <stdatomic.h>

#include <stdio.h>
#include <stdarg.h>
#include <alloca.h>

#define FALSE  0
#define TRUE   1

// Structures

typedef struct TransmissionContext* TransmissionReference;

struct FetchContext
{
  CURLM* multi;
  CURLSH* share;
  struct FastRing* ring;
  struct FastRingFlusher* flusher;
  struct FastRingDescriptor* descriptor;
  atomic_int count;

#if (LIBCURL_VERSION_NUM < 0x082800)
  TransmissionReference first;
  TransmissionReference last;
#endif
};

struct TransmissionContext
{
  struct FetchContext* fetch;

  CURL* easy;
  int state;

  char* buffer;
  size_t length;
  size_t capacity;

  HandleFetchData function;
  void* parameter1;
  void* parameter2;

#if (LIBCURL_VERSION_NUM < 0x082800)
  TransmissionReference previous;
  TransmissionReference next;
#endif
};

#ifdef BLOCK_SIZE
#undef BLOCK_SIZE
#endif

#define BLOCK_SIZE  10240

// Helpers

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

static void HandleSocketEvent(int handle, uint32_t flags, void* data, uint64_t options);
static void HandleTimeoutEvent(struct FastRingDescriptor* descriptor);
static void HandleFlushEvent(void* closure, int reason);

// CURL I/O Functions

static void TouchTransmissionQueue(struct FetchContext* fetch)
{
  if (fetch->flusher == NULL)
  {
    // HandleFlushEvent should be only set once per cycle
    fetch->flusher = SetFastRingFlushHandler(fetch->ring, HandleFlushEvent, fetch);
  }
}

static void HandleTimeoutEvent(struct FastRingDescriptor* descriptor)
{
  struct FetchContext* fetch;
  int count;

  fetch             = (struct FetchContext*)descriptor->closure;
  fetch->descriptor = NULL;

  curl_multi_socket_action(fetch->multi, CURL_SOCKET_TIMEOUT, 0, &count);
  TouchTransmissionQueue(fetch);
}

static int HandleSocketCompletion(struct FastRingDescriptor* descriptor, struct io_uring_cqe* completion, int reason)
{
  struct FetchContext* fetch;
  int count;
  int mask;

  if ((reason == RING_REASON_COMPLETE) &&
      (fetch   = (struct FetchContext*)descriptor->closure))
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
  struct FetchContext* fetch;
  struct FastRingDescriptor* descriptor;

  fetch      = (struct FetchContext*)data1;
  descriptor = (struct FastRingDescriptor*)data2;

  if (descriptor != NULL)
  {
    if (operation == CURL_POLL_REMOVE)
    {
      curl_multi_assign(fetch->multi, handle, NULL);
      descriptor->closure = NULL;
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
  struct FetchContext* fetch;

  fetch             = (struct FetchContext*)data;
  fetch->descriptor = SetFastRingTimeout(fetch->ring, fetch->descriptor, timeout, 0, HandleTimeoutEvent, fetch);
  
  return 0;
}

static size_t HandleWrite(void* buffer, size_t size, size_t count, void* data)
{
  struct TransmissionContext* transmission;
  curl_off_t capacity;
  size_t length;
  char* pointer;

  transmission  = (struct TransmissionContext*)data;
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

  if (length > transmission->capacity)
  {
    pointer                = transmission->buffer;
    transmission->capacity = length + BLOCK_SIZE + 1;
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
    pointer               = transmission->buffer + transmission->length;
    transmission->length  = length;
    memcpy(pointer, buffer, size);
  }

  return size;
}

// Routines

static void ReleaseTransmissionContext(struct TransmissionContext* transmission)
{
  struct FetchContext* fetch;
  char* location;
  char* error;
  long code;

  fetch = transmission->fetch;

  if (transmission->function != NULL)
  {
    code     = 0;
    location = NULL;

    switch (transmission->state)
    {
      case CURLE_OK:
        curl_easy_getinfo(transmission->easy, CURLINFO_RESPONSE_CODE, &code);
        if (transmission->buffer != NULL)  transmission->buffer[transmission->length] = '\0';
        transmission->function(code, transmission->easy, transmission->buffer, transmission->length, transmission->parameter1, transmission->parameter2);
        break;

      case TRANSMISSION_INCOMPLETE:
        code = transmission->state;
        transmission->function(code, transmission->easy, NULL, 0, transmission->parameter1, transmission->parameter2);
        break;

      default:
        code  = - transmission->state;
        error = (char*)curl_easy_strerror(transmission->state);
        transmission->function(code, transmission->easy, error, 0, transmission->parameter1, transmission->parameter2);
        break;
    }
  }

#if (LIBCURL_VERSION_NUM < 0x082800)
  REMOVE(fetch, transmission);
#endif

  atomic_fetch_sub_explicit(&fetch->count, 1, memory_order_relaxed);
  curl_multi_remove_handle(fetch->multi, transmission->easy);
  curl_easy_cleanup(transmission->easy);
  free(transmission->buffer);
  free(transmission);
}

static void ProceedTransmissionQueue(struct FetchContext* fetch)
{
  int count;
  struct CURLMsg* message;
  struct TransmissionContext* transmission;

  count = 0;

  while (message = curl_multi_info_read(fetch->multi, &count))
  {
    if (message->msg == CURLMSG_DONE)
    {
      curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, (char**)&transmission);
      transmission->state = message->data.result;
      ReleaseTransmissionContext(transmission);
    }
  }
}

static void HandleFlushEvent(void* closure, int reason)
{
  struct FetchContext* fetch;

  if (reason == RING_REASON_COMPLETE)
  {
    fetch          = (struct FetchContext*)closure;
    fetch->flusher = NULL;

    ProceedTransmissionQueue(fetch);
  }
}

// Public Functions

struct FetchContext* CreateFetch(struct FastRing* ring)
{
  struct FetchContext* fetch;

  fetch        = (struct FetchContext*)calloc(1, sizeof(struct FetchContext));
  fetch->ring  = ring;
  fetch->multi = curl_multi_init();
  fetch->share = curl_share_init();

  curl_multi_setopt(fetch->multi, CURLMOPT_TIMERDATA,      fetch);
  curl_multi_setopt(fetch->multi, CURLMOPT_SOCKETDATA,     fetch);
  curl_multi_setopt(fetch->multi, CURLMOPT_TIMERFUNCTION,  HandleTimerOperation);
  curl_multi_setopt(fetch->multi, CURLMOPT_SOCKETFUNCTION, HandleSocketOperation);

  curl_multi_setopt(fetch->multi, CURLMOPT_PIPELINING, CURLPIPE_HTTP1 | CURLPIPE_MULTIPLEX);
  curl_share_setopt(fetch->share, CURLSHOPT_SHARE, CURL_LOCK_DATA_COOKIE | CURL_LOCK_DATA_DNS | CURL_LOCK_DATA_SSL_SESSION | CURL_LOCK_DATA_CONNECT);

  return fetch;
}

void ReleaseFetch(struct FetchContext* fetch)
{
  struct TransmissionContext* transmission;
  CURL** handle;
  CURL** list;

  RemoveFastRingFlushHandler(fetch->ring, fetch->flusher);
  SetFastRingTimeout(fetch->ring, fetch->descriptor, -1, 0, NULL, NULL);

#if (LIBCURL_VERSION_NUM >= 0x082800)
  if (list = curl_multi_get_handles(fetch->multi))
  {
    handle = list;

    while (*handle != NULL)
    {
      curl_easy_getinfo(*handle, CURLINFO_PRIVATE, (char**)&transmission);
      ReleaseTransmissionContext(transmission);
      handle ++;
    }

    curl_free(list);
  }
#else
  while (fetch->first != NULL)
  {
    // libcurl doesn't release contexts automatically :/
    ReleaseTransmissionContext(fetch->last);
  }
#endif

  curl_share_cleanup(fetch->share);
  curl_multi_cleanup(fetch->multi);

  free(fetch);
}

struct TransmissionContext* MakeExtendedFetchTransmission(struct FetchContext* fetch, CURL* easy, HandleFetchData function, void* parameter1, void* parameter2)
{
  struct TransmissionContext* transmission;


  transmission        = (struct TransmissionContext*)calloc(1, sizeof(struct TransmissionContext));
  transmission->fetch = fetch;
  transmission->easy  = easy;

  transmission->state  = TRANSMISSION_INCOMPLETE;

  curl_easy_setopt(easy, CURLOPT_SHARE, fetch->share);
  curl_easy_setopt(easy, CURLOPT_PRIVATE, transmission);
  curl_easy_setopt(easy, CURLOPT_DNS_CACHE_TIMEOUT, 600);
  curl_easy_setopt(easy, CURLOPT_ALTSVC, "");

  if (function != NULL)
  {
    transmission->function   = function;
    transmission->parameter1 = parameter1;
    transmission->parameter2 = parameter2;

    curl_easy_setopt(easy, CURLOPT_WRITEDATA, transmission);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, HandleWrite);
  }

#if (LIBCURL_VERSION_NUM < 0x082800)
  APPEND(fetch, transmission);
#endif

  atomic_fetch_add_explicit(&fetch->count, 1, memory_order_relaxed);
  curl_multi_add_handle(fetch->multi, transmission->easy);

  return transmission;
}

struct TransmissionContext* MakeSimpleFetchTransmission(struct FetchContext* fetch, const char* location, struct curl_slist* headers, const char* token, const char* data, size_t length, HandleFetchData function, void* parameter1, void* parameter2)
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

  return MakeExtendedFetchTransmission(fetch, easy, function, parameter1, parameter2);
}

void CancelFetchTransmission(struct TransmissionContext* transmission)
{
  transmission->function = NULL;
  ReleaseTransmissionContext(transmission);
}

int GetFetchTransmissionCount(struct FetchContext* fetch)
{
  return atomic_load_explicit(&fetch->count, memory_order_relaxed);
}

struct curl_slist* AppendFetchHeader(struct curl_slist* list, int size, const char* format, ...)
{
  va_list arguments;
  char* buffer;

  buffer = (char*)alloca(size);

  va_start(arguments, format);
  vsnprintf(buffer, size, format, arguments);
  va_end(arguments);

  return curl_slist_append(list, buffer);
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

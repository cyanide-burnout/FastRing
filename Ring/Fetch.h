#ifndef FETCH_H
#define FETCH_H

#include "FastRing.h"

#include <curl/curl.h>

#ifdef __cplusplus
extern "C"
{
#endif

struct Fetch;
struct FetchTransmission;

#define FETCH_OPTION_HANDLE_CONTENT    (1 << 0)
#define FETCH_OPTION_SET_HANDLER_DATA  (1 << 1)

#define FETCH_STATUS_INCOMPLETE  -1000
#define FETCH_STATUS_CANCELLED   -1001

#define TRANSMISSION_INCOMPLETE  FETCH_STATUS_INCOMPLETE

typedef void (*HandleFetchFunction)(struct FetchTransmission* transmission, CURL* easy, int code, char* data, size_t length, void* parameter1, void* parameter2);

struct Fetch
{
  CURLM* multi;
  CURLSH* share;
  struct FastRing* ring;
  struct FastRingFlusher* flusher;
  struct FastRingDescriptor* descriptor;
};

struct FetchTransmission
{
  struct Fetch* fetch;
  CURL* easy;

  int state;
  uint32_t condition;

  HandleFetchFunction function;
  void* parameter1;
  void* parameter2;
  int option;

  char* buffer;
  size_t length;
  size_t capacity;
};


struct Fetch* CreateFetch(struct FastRing* ring);
void ReleaseFetch(struct Fetch* fetch);

int GetFetchTransmissionCount(struct Fetch* fetch);

struct FetchTransmission* MakeExtendedFetchTransmission(struct Fetch* fetch, struct FetchTransmission* transmission, CURL* easy, int option, HandleFetchFunction function, void* parameter1, void* parameter2);
struct FetchTransmission* MakeSimpleFetchTransmission(struct Fetch* fetch, const char* location, struct curl_slist* headers, const char* token, const char* data, size_t length, HandleFetchFunction function, void* parameter1, void* parameter2);
void CancelFetchTransmission(struct FetchTransmission* transmission);
void TouchFetchTransmission(struct FetchTransmission* transmission);

int AppendFetchParameter(CURLU* location, int size, const char* format, ...);
struct curl_slist* AppendFetchList(struct curl_slist* list, int size, const char* format, ...);
struct curl_slist* MakeFetchConnectAddress(const struct sockaddr* address);

int HandleFetchDebug(CURL* easy, curl_infotype type, char* data, size_t size, void* closure);

// curl_easy_setopt(easy, CURLOPT_VERBOSE, 1L);
// curl_easy_setopt(easy, CURLOPT_DEBUGDATA, syslog);
// curl_easy_setopt(easy, CURLOPT_DEBUGFUNCTION, HandleFetchDebug);

#ifdef __cplusplus
}
#endif

#endif

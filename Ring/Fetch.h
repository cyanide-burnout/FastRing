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

#define FETCH_STORAGE_SIZE  128

#define FETCH_OPTION_HANDLE_CONTENT    (1 << 0)
#define FETCH_OPTION_SET_HANDLER_DATA  (1 << 1)

#define FETCH_STATUS_INCOMPLETE  -1000
#define FETCH_STATUS_CANCELLED   -1001

#define TRANSMISSION_INCOMPLETE  FETCH_STATUS_INCOMPLETE

typedef void (*HandleFetchFunction)(struct FetchTransmission* transmission, CURL* easy, int code, char* data, size_t length, void* parameter1, void* parameter2);

struct Fetch* CreateFetch(struct FastRing* ring);
void ReleaseFetch(struct Fetch* fetch);

struct FetchTransmission* MakeExtendedFetchTransmission(struct Fetch* fetch, CURL* easy, int option, HandleFetchFunction function, void* parameter1, void* parameter2);
struct FetchTransmission* MakeSimpleFetchTransmission(struct Fetch* fetch, const char* location, struct curl_slist* headers, const char* token, const char* data, size_t length, HandleFetchFunction function, void* parameter1, void* parameter2);

struct FastRing* GetFetchTransmissionRing(struct FetchTransmission* transmission);
void* GetFetchTransmissionParameter1(struct FetchTransmission* transmission);
void* GetFetchTransmissionParameter2(struct FetchTransmission* transmission);
void* GetFetchTransmissionStorage(struct FetchTransmission* transmission);
CURL* GetFetchTransmissionHandle(struct FetchTransmission* transmission);
void TouchFetchTransmission(struct FetchTransmission* transmission);

void CancelFetchTransmission(struct FetchTransmission* transmission);
int GetFetchTransmissionCount(struct Fetch* fetch);

int AppendFetchParameter(CURLU* location, int size, const char* format, ...);
struct curl_slist* AppendFetchList(struct curl_slist* list, int size, const char* format, ...);
struct curl_slist* MakeFetchConnectAddress(const struct sockaddr* address);

#ifdef __cplusplus
}
#endif

#endif

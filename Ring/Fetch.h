#ifndef FETCH_H
#define FETCH_H

#include "FastRing.h"

#include <curl/curl.h>

#ifdef __cplusplus
extern "C"
{
#endif

#ifndef Fetch
typedef void Fetch;
typedef void FetchTransmission;
#endif

#define TRANSMISSION_INCOMPLETE  -1000

typedef void (*HandleFetchData)(int code, CURL* easy, char* data, size_t length, void* parameter1, void* parameter2);

Fetch* CreateFetch(struct FastRing* ring);
void ReleaseFetch(Fetch* fetch);

FetchTransmission* MakeExtendedFetchTransmission(Fetch* fetch, CURL* easy, HandleFetchData function, void* parameter1, void* parameter2);
FetchTransmission* MakeSimpleFetchTransmission(Fetch* fetch, const char* location, struct curl_slist* headers, const char* token, const char* data, size_t length, HandleFetchData function, void* parameter1, void* parameter2);
void CancelFetchTransmission(FetchTransmission* transmission);
int GetFetchTransmissionCount(Fetch* fetch);

struct curl_slist* AppendFetchHeader(struct curl_slist* list, int size, const char* format, ...);
int AppendFetchParameter(CURLU* location, int size, const char* format, ...);

#ifdef __cplusplus
}
#endif

#endif

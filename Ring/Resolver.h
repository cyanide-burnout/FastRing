#ifndef RESOLVER_H
#define RESOLVER_H

#include <ares.h>

#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "FastRing.h"

#ifdef __cplusplus
extern "C"
{
#endif

// Bindings to FastPoll

struct ResolverState
{
  ares_channel channel;
  struct FastRing* ring;
  struct FastRingDescriptor* descriptor;
};

struct ResolverState* CreateResolver(struct FastRing* ring);
void UpdateResolverTimer(struct ResolverState* state);
void ReleaseResolver(struct ResolverState* state);

// Extended address handling

#define RESOLVER_STATUS_PROGRESS   -1
#define RESOLVER_STATUS_SUCCESS    ARES_SUCCESS

union HostAddress
{
  char value[0];
  struct in_addr v4;
  struct in6_addr v6;
};

struct HostEntryData
{
  int count;                  // Count of received records
  int number;                 // Current record number
  int status;                 // Status of change
  uint16_t family;            // Address family
  uint32_t control;           // Checksum of response
  union HostAddress address;  // Address of selected record
};

struct HostEntryIterator
{
  int type;                      // Active record type
  int count;                     // Count of awaiting responses (0..2)
  char* list[2];                 // List of addresses to emulate hostent
  struct HostEntryData data[2];  // Data storage for IPv4 and IPv6
};

void ClearHostEntryIterator(struct HostEntryIterator* iterator);
void ResolveHostAddress(struct ResolverState* state, const char* name, int family, ares_host_callback callback, void* argument, struct HostEntryIterator* iterator);
void ValidateHostEntry(int* status, struct hostent* entry, struct HostEntryIterator* iterator);
void FillSocketAddress(struct sockaddr* address, struct hostent* entry, int port, uint32_t option, struct HostEntryIterator* iterator);
void FillHostEntry(struct HostEntryIterator* iterator, struct hostent* entry);

#ifdef __cplusplus
}
#endif

#endif

#include "Resolver.h"

#include <alloca.h>
#include <malloc.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>

#include "CRC32C.h"

#define HANDLE_READ                (RING_POLL_READ | RING_POLL_ERROR | RING_POLL_HANGUP)
#define HANDLE_WRITE               (RING_POLL_WRITE)

#define ENTRY_FLAG_DATA            (1 << 0)
#define ENTRY_FLAG_NEW             (1 << 1)

#define ENTRY_STATUS_EMPTY         0
#define ENTRY_STATUS_HAS_OLD_DATA  (ENTRY_FLAG_DATA)
#define ENTRY_STATUS_HAS_NEW_DATA  (ENTRY_FLAG_DATA | ENTRY_FLAG_NEW)

#define PAIR(value1, value2)       (value1 << 8) | (value2 << 0)

// Bindings to FastPoll

static void HandleSocketEvent(int handle, uint32_t flags, void* closure, uint64_t options)
{
  struct ResolverState* state;
  int handle1;
  int handle2;

  state   = (struct ResolverState*)closure;
  handle1 = handle | ARES_SOCKET_BAD * ((flags & HANDLE_READ)  == 0);
  handle2 = handle | ARES_SOCKET_BAD * ((flags & HANDLE_WRITE) == 0);

  ares_process_fd(state->channel, handle1, handle2);
  UpdateResolverTimer(state);
}

static void HandleTimerEvent(struct FastRingDescriptor* descriptor)
{
  struct ResolverState* state;

  state             = (struct ResolverState*)descriptor->closure;
  state->descriptor = NULL;

  ares_process_fd(state->channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
  UpdateResolverTimer(state);
}

static void ManageResolverHandler(void* data, ares_socket_t handle, int readable, int writable)
{
  struct ResolverState* state;
  uint64_t flags;

  state = (struct ResolverState*)data;
  flags = ((!!readable) * (HANDLE_READ)) | ((!!writable) * (HANDLE_READ | HANDLE_WRITE));

  ManageFastRingPoll(state->ring, handle, flags, HandleSocketEvent, data);
}

struct ResolverState* CreateResolver(struct FastRing* ring)
{
  struct ResolverState* state;
  struct ares_options options;

  state = (struct ResolverState*)calloc(1, sizeof(struct ResolverState));

  options.flags              = ARES_FLAG_STAYOPEN;
  options.sock_state_cb      = ManageResolverHandler;
  options.sock_state_cb_data = state;

  if (ares_init_options(&state->channel, &options, ARES_OPT_FLAGS | ARES_OPT_SOCK_STATE_CB) != ARES_SUCCESS)
  {
    free(state);
    return NULL;
  }

  state->ring       = ring;
  state->descriptor = NULL;

  return state;
}

void UpdateResolverTimer(struct ResolverState* state)
{
  struct timeval* interval;

  interval          = ares_timeout(state->channel, NULL, (struct timeval*)alloca(sizeof(struct timeval)));
  state->descriptor = SetFastRingCertainTimeout(state->ring, state->descriptor, interval, 0, HandleTimerEvent, state);
}

void ReleaseResolver(struct ResolverState* state)
{
  if (state != NULL)
  {
    SetFastRingTimeout(state->ring, state->descriptor, -1, 0, NULL, NULL);
    ares_destroy(state->channel);
    free(state);
  }
}

// Extended address handling

static void FillEntryData(struct hostent* entry, struct HostEntryData* data)
{
  char** pointer;
  uint32_t control;

  data->count  = 0;
  data->status = ENTRY_STATUS_EMPTY;

  if ((entry->h_addr_list    != NULL) &&
      (entry->h_addr_list[0] != NULL))
  {
    control      = 0;
    pointer      = entry->h_addr_list;
    data->family = entry->h_addrtype;

    while (*pointer != NULL)
    {
      control = GetCRC32C((uint8_t*)*pointer, entry->h_length, control);
      pointer     ++;
      data->count ++;
    }

    if (data->control != control)
    {
      data->number  = 0;
      data->control = control;
      data->status  = ENTRY_STATUS_HAS_NEW_DATA;
    }
    else
    {
      data->number ++;
      data->number %= data->count;
      data->status  = ENTRY_STATUS_HAS_OLD_DATA;
    }

    memcpy(data->address.value, entry->h_addr_list[data->number], entry->h_length);
  }
}

static void FillEntryIterator(struct hostent* entry, struct HostEntryIterator* iterator)
{
  switch (entry->h_addrtype)
  {
    case AF_INET:
      FillEntryData(entry, iterator->data + 0);
      break;

    case AF_INET6:
      FillEntryData(entry, iterator->data + 1);
      break;
  }
}

static struct HostEntryData* GetHostEntryData(struct HostEntryIterator* iterator)
{
  switch (PAIR(iterator->data[0].status, iterator->data[1].status))
  {
    case PAIR(ENTRY_STATUS_EMPTY, ENTRY_STATUS_EMPTY):
      return NULL;

    case PAIR(ENTRY_STATUS_HAS_OLD_DATA, ENTRY_STATUS_EMPTY):
    case PAIR(ENTRY_STATUS_HAS_NEW_DATA, ENTRY_STATUS_EMPTY):
    case PAIR(ENTRY_STATUS_HAS_NEW_DATA, ENTRY_STATUS_HAS_OLD_DATA):
    case PAIR(ENTRY_STATUS_HAS_NEW_DATA, ENTRY_STATUS_HAS_NEW_DATA):
      iterator->type = 0;
      return iterator->data + iterator->type;

    case PAIR(ENTRY_STATUS_EMPTY,        ENTRY_STATUS_HAS_OLD_DATA):
    case PAIR(ENTRY_STATUS_EMPTY,        ENTRY_STATUS_HAS_NEW_DATA):
    case PAIR(ENTRY_STATUS_HAS_OLD_DATA, ENTRY_STATUS_HAS_NEW_DATA):
      iterator->type = 1;
      return iterator->data + iterator->type;

    case PAIR(ENTRY_STATUS_HAS_OLD_DATA, ENTRY_STATUS_HAS_OLD_DATA):
      iterator->type ^= 1;
      return iterator->data + iterator->type;
  }
}

void ClearHostEntryIterator(struct HostEntryIterator* iterator)
{
  memset(iterator, 0, sizeof(struct HostEntryIterator));
}

void ResolveHostAddress(struct ResolverState* state, const char* name, int family, ares_host_callback callback, void* argument, struct HostEntryIterator* iterator)
{
  size_t value;

  if ((strncmp(name, "ipv4:", 5) == 0) ||
      (strncmp(name, "ipv6:", 5) == 0))
  {
    value  = name[3];
    value |= family << 8;

    switch (value)
    {
      case '4':  family = AF_INET;   break;
      case '6':  family = AF_INET6;  break;
    }

    name += 5;
  }

  if (family == AF_UNSPEC)
  {
    value = strlen(name);

    if ((value >= 7) &&
        (value <= 15) &&
        (value == strspn(name, "0123456789.")))
    {
      // 0.0.0.0 - 255.255.255.255
      family = AF_INET;
    }

    if ((value >= 3) &&
        (value <= INET6_ADDRSTRLEN) &&
        (value == strspn(name, "0123456789abcdef:")))
    {
      // ::1
      family = AF_INET6;
    }
  }

  if (iterator == NULL)
  {
    ares_gethostbyname(state->channel, name, family, callback, argument);
    UpdateResolverTimer(state);
    return;
  }

  iterator->data[0].count = 0;
  iterator->data[1].count = 0;

  switch (family)
  {
    case AF_INET:
    case AF_INET6:
      iterator->count = 1;
      ares_gethostbyname(state->channel, name, family, callback, argument);
      UpdateResolverTimer(state);
      return;

    case AF_UNSPEC:
      iterator->count = 2;
      ares_gethostbyname(state->channel, name, AF_INET,  callback, argument);
      ares_gethostbyname(state->channel, name, AF_INET6, callback, argument);
      UpdateResolverTimer(state);
      return;
  }
}

void ValidateHostEntry(int* status, struct hostent* entry, struct HostEntryIterator* iterator)
{
  if ((*status == ARES_SUCCESS) &&
      ((entry                 == NULL) ||
       (entry->h_addr_list    == NULL) ||
       (entry->h_addr_list[0] == NULL)))
  {
    // In case of CNAME A-RES may return ARES_SUCCESS with empty list
    *status = ARES_ENOTFOUND;
  }

  if (iterator != NULL)
  {
    iterator->count --;

    if (entry != NULL)
    {
      // In case of A-RES error entry might be NULL
      FillEntryIterator(entry, iterator);
    }

    if (iterator->count > 0)
    {
      *status = RESOLVER_STATUS_PROGRESS;
      return;
    }

    if ((iterator->data[0].count > 0) ||
        (iterator->data[1].count > 0))
    {
      *status = RESOLVER_STATUS_SUCCESS;
      return;
    }
  }
}

void FillSocketAddress(struct sockaddr* address, struct hostent* entry, int port, uint32_t option, struct HostEntryIterator* iterator)
{
  int family;
  union HostAddress* value;
  struct HostEntryData* data;

  struct sockaddr_in* address1;
  struct sockaddr_in6* address2;

  family = AF_UNSPEC;

  if (entry != NULL)
  {
    family = entry->h_addrtype;
    value  = (union HostAddress*)entry->h_addr_list[0];
  }

  if (iterator != NULL)
  {
    data   = GetHostEntryData(iterator);
    family = data->family;
    value  = &data->address;
  }

  switch (family | option)
  {
    case AF_INET:
      address1 = (struct sockaddr_in*)address;
      address1->sin_family      = AF_INET;
      address1->sin_port        = htons(port);
      address1->sin_addr.s_addr = value->v4.s_addr;
      break;

    case AF_INET | ARES_AI_V4MAPPED:
      address2 = (struct sockaddr_in6*)address;
      address2->sin6_family                      = AF_INET6;
      address2->sin6_flowinfo                    = 0;
      address2->sin6_scope_id                    = 0;
      address2->sin6_port                        = htons(port);
      address2->sin6_addr.__in6_u.__u6_addr32[0] = 0;
      address2->sin6_addr.__in6_u.__u6_addr32[1] = 0;
      address2->sin6_addr.__in6_u.__u6_addr16[4] = 0;
      address2->sin6_addr.__in6_u.__u6_addr16[5] = 0xffff;
      address2->sin6_addr.__in6_u.__u6_addr32[3] = value->v4.s_addr;
      break;

    case AF_INET6:
    case AF_INET6 | ARES_AI_V4MAPPED:
      address2 = (struct sockaddr_in6*)address;
      address2->sin6_family                      = AF_INET6;
      address2->sin6_flowinfo                    = 0;
      address2->sin6_scope_id                    = 0;
      address2->sin6_port                        = htons(port);
      address2->sin6_addr.__in6_u.__u6_addr32[0] = value->v6.__in6_u.__u6_addr32[0];
      address2->sin6_addr.__in6_u.__u6_addr32[1] = value->v6.__in6_u.__u6_addr32[1];
      address2->sin6_addr.__in6_u.__u6_addr32[2] = value->v6.__in6_u.__u6_addr32[2];
      address2->sin6_addr.__in6_u.__u6_addr32[3] = value->v6.__in6_u.__u6_addr32[3];
      break;
  }
}

void FillHostEntry(struct HostEntryIterator* iterator, struct hostent* entry)
{
  static const int const lengths[] =
  {
    sizeof(struct in_addr),
    sizeof(struct in6_addr)
  };

  struct HostEntryData* data;

  memset(entry, 0, sizeof(struct hostent));
  data = GetHostEntryData(iterator);

  if (data != NULL)
  {
    iterator->list[0]  = data->address.value;
    entry->h_addrtype  = data->family;
    entry->h_addr_list = iterator->list;
    entry->h_length    = lengths[data->family == AF_INET6];
  }
}

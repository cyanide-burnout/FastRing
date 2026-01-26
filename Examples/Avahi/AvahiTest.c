#include <signal.h>
#include <string.h>
#include <malloc.h>
#include <stdio.h>

#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/alternative.h>
#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>

#include "FastAvahiPoll.h"

#define STATE_RUNNING  -1

atomic_int state = { STATE_RUNNING };

static void HandleSignal(int signal)
{
  // Interrupt main loop in case of interruption signal
  atomic_store_explicit(&state, 0, memory_order_relaxed);
}

// Browser

static void HandleResolverEvent(AvahiServiceResolver* resolver, AvahiIfIndex interface, AvahiProtocol protocol, AvahiResolverEvent event,
  const char* name, const char* type, const char* domain, const char* host, const AvahiAddress* address, uint16_t port,
  AvahiStringList* text, AvahiLookupResultFlags flags, void* closure)
{
  char buffer[AVAHI_ADDRESS_STR_MAX];
  char* value;

  switch (event)
  {
    case AVAHI_RESOLVER_FAILURE:
      printf("(Resolver) Failed to resolve service '%s' of type '%s' in domain '%s': %s\n", name, type, domain, avahi_strerror(avahi_client_errno(avahi_service_resolver_get_client(resolver))));
      break;

    case AVAHI_RESOLVER_FOUND:
      avahi_address_snprint(buffer, AVAHI_ADDRESS_STR_MAX, address);
      value = avahi_string_list_to_string(text);

      printf(
        "(Resolver) Service '%s' of type '%s' in domain '%s':\n"
        "\t%s:%u (%s)\n"
        "\tTXT=%s\n"
        "\tcookie is %u\n"
        "\tis_local: %i\n"
        "\tour_own: %i\n"
        "\twide_area: %i\n"
        "\tmulticast: %i\n"
        "\tcached: %i\n",
        name, type, domain,
        host, port, buffer,
        value,
        avahi_string_list_get_service_cookie(text),
        !!(flags & AVAHI_LOOKUP_RESULT_LOCAL),
        !!(flags & AVAHI_LOOKUP_RESULT_OUR_OWN),
        !!(flags & AVAHI_LOOKUP_RESULT_WIDE_AREA),
        !!(flags & AVAHI_LOOKUP_RESULT_MULTICAST),
        !!(flags & AVAHI_LOOKUP_RESULT_CACHED));

      avahi_free(value);
      break;
  }

  avahi_service_resolver_free(resolver);
}

static void HandleBrowserEvent(AvahiServiceBrowser* browser, AvahiIfIndex interface, AvahiProtocol protocol, AvahiBrowserEvent event,
  const char* name, const char* type, const char* domain, AvahiLookupResultFlags flags, void* closure)
{
  AvahiClient* client;

  client = (AvahiClient*)closure;

  switch (event)
  {
    case AVAHI_BROWSER_FAILURE:
      printf("(Browser) %s\n", avahi_strerror(avahi_client_errno(avahi_service_browser_get_client(browser))));
      return;

    case AVAHI_BROWSER_NEW:
      printf("(Browser) NEW: service '%s' of type '%s' in domain '%s'\n", name, type, domain);

      if (avahi_service_resolver_new(client, interface, protocol, name, type, domain, AVAHI_PROTO_UNSPEC, 0, HandleResolverEvent, client) == NULL)
      {
        //
        printf("(Browser) Failed to resolve service '%s': %s\n", name, avahi_strerror(avahi_client_errno(client)));
      }

      break;

    case AVAHI_BROWSER_REMOVE:
      printf("(Browser) REMOVE: service '%s' of type '%s' in domain '%s'\n", name, type, domain);
      break;

    case AVAHI_BROWSER_ALL_FOR_NOW:
      printf("(Browser) ALL_FOR_NOW\n");
      break;

    case AVAHI_BROWSER_CACHE_EXHAUSTED:
      printf("(Browser) CACHE_EXHAUSTED\n");
      break;
  }
}

// Publisher

struct ServiceContext
{
  AvahiClient* client;
  AvahiEntryGroup* group;
  char* name;
};

static void RegisterService(struct ServiceContext* context);

static void ChangeServiceName(struct ServiceContext* context)
{
  char* name;

  name = avahi_alternative_service_name(context->name);
  avahi_free(context->name);
  context->name = name;

  printf("(Publisher) Service name collision, renaming service to '%s'\n", name);
  RegisterService(context);
}

static void HandleGroupEvent(AvahiEntryGroup* group, AvahiEntryGroupState state, void* closure)
{
  struct ServiceContext* context;

  context        = (struct ServiceContext*)closure;
  context->group = group;

  switch (state)
  {
    case AVAHI_ENTRY_GROUP_ESTABLISHED:
      printf("(Publisher) Service '%s' successfully established\n", context->name);
      break;

    case AVAHI_ENTRY_GROUP_COLLISION:
      ChangeServiceName(context);
      break;

    case AVAHI_ENTRY_GROUP_FAILURE:
      printf("(Publisher) Entry group failure: %s\n", avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(group))));
      atomic_store_explicit(&state, 0, memory_order_relaxed);
      break;

    case AVAHI_ENTRY_GROUP_UNCOMMITED:
    case AVAHI_ENTRY_GROUP_REGISTERING:
      break;
  }
}

static void RegisterService(struct ServiceContext* context)
{
  int result;

  if ( (context->group == NULL) &&
      ((context->group  = avahi_entry_group_new(context->client, HandleGroupEvent, context)) == NULL))
  {
    printf("(Publisher) avahi_entry_group_new() failed: %s\n", avahi_strerror(avahi_client_errno(context->client)));
    atomic_store_explicit(&state, 0, memory_order_relaxed);
    return;
  }

  if (avahi_entry_group_is_empty(context->group))
  {
    printf("(Publisher) Adding service '%s'\n", context->name);

    if (((result = avahi_entry_group_add_service(context->group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0, context->name, "_dummy._tcp", NULL, NULL, 651, "test=blah", NULL)) < 0) ||
        ((result = avahi_entry_group_commit(context->group)) < 0))
    {
      if (result == AVAHI_ERR_COLLISION)
      {
        ChangeServiceName(context);
        return;
      }

      printf("(Publisher) Failed to add service: %s\n", avahi_strerror(result));
      atomic_store_explicit(&state, 0, memory_order_relaxed);
      return;
    }
  }
}

static void HandleTimeoutCompletion(struct FastRingDescriptor* descriptor)
{
  struct ServiceContext* context;

  context = (struct ServiceContext*)descriptor->closure;

  if ((context->client != NULL) &&
      (avahi_client_get_state(context->client) == AVAHI_CLIENT_S_RUNNING))
  {
    //
    RegisterService(context);
  }
}

// Client

static void HandleClientEvent(AvahiClient* client, AvahiClientState state, void* closure)
{
  struct ServiceContext* context;

  context = (struct ServiceContext*)closure;

  switch (state)
  {
    case AVAHI_CLIENT_S_RUNNING:
      avahi_free(context->name);
      context->client = client;
      context->name   = avahi_strdup("TestService");
      RegisterService(context);
      break;

    case AVAHI_CLIENT_FAILURE:
      printf("Server connection failure: %s\n", avahi_strerror(avahi_client_errno(client)));
      break;
  }
}

int main()
{
  struct sigaction action;

  struct FastRing* ring;
  struct FastRingDescriptor* timeout;

  AvahiServiceBrowser* browser;
  AvahiClient* client;
  AvahiPoll* poll;
  int error;

  struct ServiceContext context;

  action.sa_handler = HandleSignal;
  action.sa_flags   = SA_NODEFER | SA_RESTART;

  sigemptyset(&action.sa_mask);

  sigaction(SIGHUP,  &action, NULL);
  sigaction(SIGINT,  &action, NULL);
  sigaction(SIGTERM, &action, NULL);
  sigaction(SIGQUIT, &action, NULL);

  memset(&context, 0, sizeof(struct ServiceContext));

  ring   = CreateFastRing(0);
  poll   = CreateFastAvahiPoll(ring);
  client = avahi_client_new(poll, 0, HandleClientEvent, &context, &error);

  if (client == NULL)
  {
    printf("Failed to create client: %s\n", avahi_strerror(error));
    ReleaseFastAvahiPoll(poll);
    ReleaseFastRing(ring);
    return 1;
  }

  printf("Started\n");

  browser = avahi_service_browser_new(client, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, "_dummy._tcp", NULL, 0, HandleBrowserEvent, client);
  timeout = SetFastRingTimeout(ring, NULL, 10000, TIMEOUT_FLAG_REPEAT, HandleTimeoutCompletion, &context);

  while ((atomic_load_explicit(&state, memory_order_relaxed) == STATE_RUNNING) &&
         (WaitForFastRing(ring, 200, NULL) >= 0));

  avahi_service_browser_free(browser);
  avahi_client_free(client);
  avahi_free(context.name);

  SetFastRingTimeout(ring, timeout, -1, 0, NULL, NULL);
  ReleaseFastAvahiPoll(poll);
  ReleaseFastRing(ring);

  printf("Stopped\n");

  return 0;
}

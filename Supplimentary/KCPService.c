#include "KCPService.h"

#include <stddef.h>
#include <alloca.h>
#include <malloc.h>
#include <string.h>
#include <errno.h>

#define KCP_RTO_MIN  100
#define KCP_RTO_MAX  60000

#define KCP_BUFFER_ALLIGNMENT  1024

// KCP congestion control

static void InitializeKCPCongestion(struct KCPCongestion* congestion)
{
  memset(congestion, 0, sizeof(struct KCPCongestion));

  congestion->mss         = KCP_DEFAULT_MSS;
  congestion->cwnd        = 1;                        // start with one segment
  congestion->ssthresh    = 2;                        // minimum slow start threshold
  congestion->incr        = congestion->mss;          //
  congestion->control.wnd = KCP_DEFAULT_WND;          // our local receive window capacity
  congestion->rmtwnd      = congestion->control.wnd;  //
  congestion->fastresend  = KCP_DEFAULT_FASTRESEND;   // fast retransmit disabled by default
  congestion->rto         = KCP_DEFAULT_RTO;          //
  congestion->interval    = KCP_DEFAULT_INTERVAL;     //
  congestion->ackthresh   = KCP_DEFAULT_ACKTHRESH;    //
}

static void SetKCPCongestionRemoteWindow(struct KCPCongestion* congestion, uint32_t wnd)
{
  congestion->rmtwnd = wnd;  // remote->wnd is the peer's advertised receive window (in segments)
}

static void HandleKCPCongestionProgress(struct KCPCongestion* congestion, uint32_t old, uint32_t new)
{
  if ((int32_t)(new - old) <= 0)
  {
    // No forward progress
    return;
  }

  // Keep shared sndUna in sync
  congestion->control.una = new;

  if (congestion->rmtwnd == 0)
  {
    // Do not grow if remote window is unknown or zero
    return;
  }

  if (congestion->cwnd < congestion->rmtwnd)
  {
    // Grow only while cwnd is below remote window

    if (congestion->cwnd < congestion->ssthresh)
    {
      // Slow start: exponential growth (approximately doubling each RTT)
      congestion->cwnd  += 1;
      congestion->incr  += congestion->mss;
    }
    else
    {
      // Congestion avoidance: additive increase, about +1 MSS per RTT
      congestion->incr  = (congestion->incr < congestion->mss) ? congestion->mss : congestion->incr;
      congestion->incr += congestion->mss * congestion->mss / congestion->incr + congestion->mss / 16;
      congestion->cwnd += (congestion->cwnd + 1) * congestion->mss <= congestion->incr;
    }

    if (congestion->cwnd > congestion->rmtwnd)
    {
      // Final clamp to remote window, same idea as in KCP
      congestion->cwnd = congestion->rmtwnd;
      congestion->incr = congestion->rmtwnd * congestion->mss;
    }
  }
}

static void HandleKCPCongestionFastResend(struct KCPCongestion* congestion)
{
  uint32_t k;
  uint32_t flight;

  flight               = congestion->control.sn - congestion->control.una;               // Flight size: number of segments currently in-flight
  congestion->ssthresh = (flight < 4) ? 2 : flight / 2;
  k                    = congestion->fastresend ? congestion->fastresend : 0x7fffffffU;  // Same behavior as Kcp.java: if fastresend is zero, use a very large value
  congestion->cwnd     = congestion->ssthresh + k;
  congestion->incr     = congestion->cwnd * congestion->mss;
}

static void HandleKCPCongestionTimeout(struct KCPCongestion* congestion)
{
  congestion->ssthresh = (congestion->cwnd < 2) ? 2 : (congestion->cwnd / 2);
  congestion->incr     = congestion->mss;
  congestion->cwnd     = 1;
}

static void HandleKCPCongestionRTTSample(struct KCPCongestion* congestion, uint32_t rtt)
{
  uint32_t rto;
  int32_t delta;

  rtt = (rtt == 0) ? 1 : rtt;

  if (congestion->srtt == 0)
  {
    congestion->srtt   = rtt;
    congestion->rttvar = rtt / 2;
  }
  else
  {
    delta               = (int32_t)rtt - (int32_t)congestion->srtt;
    congestion->srtt   += (uint32_t)(delta / 8);
    delta              *= 1 - 2 * (delta < 0);
    congestion->rttvar += (uint32_t)(((int32_t)delta - (int32_t)congestion->rttvar) / 4);
  }

  rto              = congestion->srtt + ((congestion->interval > 0) ? congestion->interval : 1);
  rto             += 4 * congestion->rttvar;
  congestion->rto  = (rto < KCP_RTO_MIN) ? KCP_RTO_MIN : (rto > KCP_RTO_MAX) ? KCP_RTO_MAX : rto;
}

static uint32_t GetKCPCongestionSendQuota(const struct KCPCongestion* congestion, uint32_t segments)
{
  uint32_t flight;
  uint32_t limit;

  flight = congestion->control.sn - congestion->control.una;
  limit  = (!congestion->nocwnd && (congestion->cwnd < congestion->rmtwnd)) ? congestion->cwnd : congestion->rmtwnd;

  if (flight < limit)
  {
    limit -= flight;
    return (limit < segments) ? limit : segments;
  }

  return 0;
}

static int UpdateKCPZeroWindowProbe(struct KCPCongestion* congestion, uint32_t ts)
{
  if (congestion->rmtwnd != 0)
  {
    congestion->pwait  = 0;
    congestion->pts    = 0;
    return 0;
  }

  if (congestion->pwait == 0)
  {
    congestion->pwait = congestion->rto;
    congestion->pwait = (congestion->pwait < 500)    ? 500    : congestion->pwait;
    congestion->pwait = (congestion->pwait > 120000) ? 120000 : congestion->pwait;
    congestion->pts   = ts + congestion->pwait;
    return 0;
  }

  if ((int32_t)(ts - congestion->pts) >= 0)
  {
    congestion->pwait   = congestion->pwait + congestion->rto;
    congestion->pwait   = (congestion->pwait > 120000) ? 120000 : congestion->pwait;
    congestion->pts     = ts + congestion->pwait;
    return 1;
  }

  return 0;
}

// KCP queue management

static int ExpandKCPQueue(struct KCPQueue* queue, uint32_t size)
{
  void* block;

  if ((size <= 1) ||
      ((size  = 1u << (32 - __builtin_clz(size - 1))) <= queue->size) ||
      ((block = realloc(queue->slots, size * sizeof(struct KCPSegment*))) == NULL))
  {
    // Fatal error
    return -1;
  }

  queue->slots  = (struct KCPSegment**)block;
  queue->head  &= queue->size - 1;
  queue->tail  &= queue->size - 1;

  if (queue->tail < queue->head)
  {
    memcpy(queue->slots + queue->size, queue->slots, queue->tail * sizeof(struct KCPSegment*));
    queue->tail += queue->size;
    queue->size  = queue->tail;
  }

  memset(queue->slots + queue->size, 0, (size - queue->size) * sizeof(struct KCPSegment*));
  queue->size = size;

  return 0;
}

static int PutIntoKCPQueue(struct KCPQueue* queue, struct KCPSegment* segment, uint32_t window)
{
  int32_t difference;
  uint32_t number;
  uint32_t mask;

  if ((queue->size == 0) &&
      (ExpandKCPQueue(queue, UINT8_MAX + 1) != 0))
  {
    // Faral error
    return -ENOMEM;
  }

  mask = queue->size - 1;

  if (queue->head == queue->tail)
  {
    queue->slots[queue->tail & mask] = segment;
    queue->tail ++;
    return 0;
  }

  difference = (int32_t)(le32toh(segment->control->sn) - le32toh(queue->slots[queue->head & mask]->control->sn));

  if (difference <= 0)
  {
    // Segment is too old
    return -EBADF;
  }

  if (difference >= window)
  {
    // Out of allowed range
    return -EFBIG;
  }

  number = difference + 1;

  if ((number > queue->size) &&
      (ExpandKCPQueue(queue, number) != 0))
  {
    // Faral error
    return -ENOMEM;
  }

  mask   = queue->size - 1;
  number = queue->head + difference;

  if (queue->slots[number & mask] != NULL)
  {
    // Duplicated segment
    return -EEXIST;
  }

  queue->slots[number & mask] = segment;

  if (number >= queue->tail)
  {
    // Move tail
    queue->tail = number + 1;
  }

  return 0;
}

static int PushIntoKCPQueue(struct KCPQueue* queue, struct KCPSegment* segment)
{
  if ((queue->size == 0) &&
      (ExpandKCPQueue(queue, UINT8_MAX + 1) != 0) ||
      ((queue->tail - queue->head) >= queue->size) &&
      (ExpandKCPQueue(queue, queue->size * 2) != 0))
  {
    // Fatal error
    return -ENOMEM;
  }

  queue->slots[(queue->tail ++) & (queue->size - 1)] = segment;
  return 0;
}

uint32_t GetKCPQueueLength(struct KCPQueue* queue)
{
  return queue->tail - queue->head;
}

// Helpers

static uint16_t GetKCPWindowSize(struct KCPQueue* queue, struct KCPCongestion* congestion)
{
  uint32_t size1;
  uint32_t size2;

  size1 = queue->tail - queue->head;
  size2 = congestion->control.wnd - size1;

  return (size1 >= congestion->control.wnd) ? 0 : (size2 > UINT16_MAX) ? UINT16_MAX : size2;
}

static struct KCPSegment* AllocateKCPSegment(struct KCPService* service)
{
  struct KCPQueue* queue;
  struct KCPSegment* segment;

  queue = &service->cache;

  if (queue->head != queue->tail)
  {
    segment = queue->slots[(queue->head ++) & (queue->size - 1)];
    memset(segment, 0, sizeof(struct KCPSegment));
    return segment;
  }

  return (struct KCPSegment*)calloc(1, sizeof(struct KCPSegment));
}

static void ReleaseKCPSegment(struct KCPService* service, struct KCPSegment* segment)
{
  struct KCPQueue* queue;

  if (segment != NULL)
  {
    queue = &service->cache;

    if ((segment->release != NULL) &&
        (segment->closure != NULL))
    {
      //
      segment->release(segment->closure);
    }

    if ((queue->size == 0) &&
        (ExpandKCPQueue(queue, UINT8_MAX + 1) != 0) ||
        ((queue->tail - queue->head) >= queue->size) &&
        (ExpandKCPQueue(queue, queue->size * 2) != 0))
    {
      free(segment);
      return;
    }

    queue->slots[(queue->tail ++) & (queue->size - 1)] = segment;
  }
}

static int TransmitKCPControlSegment(struct KCPConversation* conversation, uint8_t cmd, uint16_t wnd, uint32_t ts, uint32_t sn, uint32_t una)
{
  const struct KCPFormat* format;
  struct KCPTransmitter* transmitter;
  struct KCPCongestion* congestion;
  struct KCPService* service;
  struct KCPSegment* segment;
  struct KCPControl* control;
  uint8_t* buffer;
  uint32_t size;
  int result;

  format      = conversation->format;
  service     = conversation->service;
  congestion  = &conversation->congestion;
  transmitter = service->transmitter;

  size    = format->propose((struct sockaddr*)&conversation->key.address, 0);
  buffer  = transmitter->allocate(transmitter->closure, size);
  segment = AllocateKCPSegment(service);

  if ((buffer  == NULL) ||
      (segment == NULL))
  {
    transmitter->release(buffer);
    ReleaseKCPSegment(service, segment);
    return -ENOMEM;
  }

  segment->closure = buffer;
  segment->release = transmitter->release;

  format->prepare(buffer, &conversation->key, segment, 0);

  control      = segment->control;
  control->cmd = cmd;
  control->frg = 0;
  control->wnd = wnd;
  control->ts  = ts;
  control->sn  = sn;
  control->una = una;

  format->compose(segment);

  result = transmitter->transmit(transmitter->closure, (struct sockaddr*)&conversation->key.address, segment->packet, segment->size);

  ReleaseKCPSegment(service, segment);
  return result;
}

static void CallKCPHandler(struct KCPService* service, struct KCPConversation* conversation, int event, uint8_t* data, size_t length)
{
  struct KCPHandler* handler;

  conversation->state |= KCP_CONVERSATION_GUARD;
  handler              = service->handler;
  handler->handle(handler->closure, conversation, event, data, length);
  conversation->state &= ~KCP_CONVERSATION_GUARD;
}

// Receiver

static int HandleIncomingKCPMessage(struct KCPService* service, struct KCPConversation* conversation, size_t count, size_t length)
{
  struct KCPSegment* segment;
  struct KCPQueue* queue;
  uint8_t* buffer;
  uint32_t mask;
  size_t size;

  buffer  = service->buffer;
  queue   = &conversation->inbound;
  mask    = queue->size - 1;
  segment = queue->slots[queue->head & mask];

  if (segment->state & KCP_SEGMENT_ANCHOR)
  {
    ReleaseKCPSegment(service, segment);
    queue->slots[queue->head & mask] = NULL;
    segment = queue->slots[(++ queue->head) & mask];
  }

  if (count == 1)
  {
    segment->state |= KCP_SEGMENT_ANCHOR;
    CallKCPHandler(service, conversation, KCP_EVENT_RECEIVE, segment->data, length);
    return 0;
  }

  if (length >= service->size)
  {
    size   = (length + KCP_BUFFER_ALLIGNMENT) & ~(KCP_BUFFER_ALLIGNMENT - 1);
    buffer = (uint8_t*)realloc(service->buffer, size);

    if (buffer == NULL)
    {
      // Fatal error
      return -ENOMEM;
    }

    service->buffer = buffer;
    service->size   = size;
  }

  while (-- count)
  {
    memcpy(buffer, segment->data, segment->length);
    buffer += segment->length;

    ReleaseKCPSegment(service, segment);
    queue->slots[queue->head & mask] = NULL;
    segment = queue->slots[(++ queue->head) & mask];
  }

  memcpy(buffer, segment->data, segment->length);

  segment->state |= KCP_SEGMENT_ANCHOR;
  CallKCPHandler(service, conversation, KCP_EVENT_RECEIVE, service->buffer, length);
  return 0;
}

static int HandleIncomingKCPSegment(struct KCPService* service, struct KCPConversation* conversation, struct KCPSegment* segment)
{
  struct KCPCongestion* congestion;
  struct KCPControl* control;
  struct KCPQueue* queue;
  uint32_t number;
  uint32_t entry;
  uint32_t time;
  uint32_t mask;
  uint16_t size;
  size_t length;
  size_t count;
  int result;
  int guard;

  congestion = &conversation->congestion;
  control    = segment->control;
  result     = 0;
  time       = conversation->time.tv_sec * 1000u + conversation->time.tv_nsec / 1000000u;

  SetKCPCongestionRemoteWindow(congestion, le16toh(control->wnd));

  switch (control->cmd)
  {
    case KCP_CMD_PUSH:
      if ((result = PutIntoKCPQueue(&conversation->inbound, segment, congestion->control.wnd)) < 0)
      {
        if ((result == -EBADF) ||
            (result == -EEXIST))
        {
          congestion->acknowledge.cmd = KCP_CMD_PUSH;
          congestion->acknowledge.ts  = control->ts;
          congestion->acknowledge.sn  = control->sn;
          congestion->ackdue          = time;
          result                      = 0;
        }

        ReleaseKCPSegment(service, segment);
        break;
      }

      queue   = &conversation->inbound;
      mask    = queue->size - 1;
      entry   = queue->head;
      control = NULL;
      number  = 0;
      length  = 0;
      count   = 0;

      while ((result  == 0) &&
             (entry   != queue->tail) &&
             (segment  = queue->slots[entry & mask]))
      {
        if (~segment->state & KCP_SEGMENT_ANCHOR)
        {
          control  = segment->control;
          number   = control->sn;
          length  += segment->length;
          count   ++;

          if (control->frg == 0)
          {
            result = HandleIncomingKCPMessage(service, conversation, count, length);
            length = 0;
            count  = 0;
          }
        }

        entry ++;
      }

      entry = le32toh(number) + 1;

      if ((control != NULL) &&
          (entry   != congestion->rcvnxt))
      {
        congestion->rcvnxt         = entry;
        congestion->acknowledge.ts = control->ts;
        congestion->acknowledge.sn = control->sn;

        if ((int32_t)(queue->tail - queue->head) >= (int32_t)congestion->ackthresh)
        {
          congestion->acknowledge.cmd = KCP_CMD_PUSH;
          congestion->ackdue          = time;
        }

        if (congestion->acknowledge.cmd == 0)
        {
          congestion->acknowledge.cmd = KCP_CMD_ACK;
          congestion->ackdue          = time + congestion->interval;
        }
      }

      break;

    case KCP_CMD_ACK:
      number = control->una;
      entry  = control->sn;
      queue  = &conversation->outbound;
      mask   = queue->size - 1;
      guard  = 0;

      ReleaseKCPSegment(service, segment);
      HandleKCPCongestionProgress(congestion, congestion->control.una, le32toh(number));

      if ((int32_t)(le32toh(number) - congestion->control.una) >= 0)
      {
        while ((queue->head != queue->tail) &&
               (segment      = queue->slots[queue->head & mask]) &&
               (control      = segment->control) &&
               (control->sn != number))
        {
          guard |= control->sn == entry;  // Set fast-track guard, wnen sn reached
          HandleKCPCongestionRTTSample(congestion, time - le32toh(control->ts));
          ReleaseKCPSegment(service, segment);
          queue->slots[queue->head & mask] = NULL;
          queue->head ++;
        }

        if (queue->head == queue->tail)
        {
          //
          CallKCPHandler(service, conversation, KCP_EVENT_RESUME, NULL, 0);
        }
      }

      if ((guard == 0) &&
          (congestion->fastresend != 0) &&
          ((int32_t)(le32toh(entry) - congestion->control.sn) < 0) &&
          ((int32_t)(le32toh(entry) - congestion->control.una) >= 0))
      {
        number = entry;
        entry  = queue->head;

        while ((entry != queue->tail) &&
               (segment      = queue->slots[entry & mask]) &&
               (control      = segment->control) &&
               (control->sn != number))
        {
          entry          ++;
          segment->track ++;

          if ((segment->track >= congestion->fastresend) &&
              (segment->tries > 0))
          {
            HandleKCPCongestionFastResend(congestion);
            segment->track = 0;
          }
        }
      }

      break;

    case KCP_CMD_WASK:
      ReleaseKCPSegment(service, segment);
      size = GetKCPWindowSize(&conversation->inbound, congestion);
      TransmitKCPControlSegment(conversation, KCP_CMD_WINS, htole16(size), htole32(time), 0, htole32(congestion->rcvnxt));
      break;

    case KCP_CMD_WINS:
      ReleaseKCPSegment(service, segment);
      break;

    default:
      ReleaseKCPSegment(service, segment);
      result = -EINVAL;
  }

  return result;
}

int HandleKCPPacket(struct KCPService* service, const struct KCPFormat* format, struct KCPConversation** reference, struct timespec* time, struct sockaddr* address, void* packet, uint32_t size, struct KCPPoint* point, AcquireKCPClosure acquire, ReleaseKCPClosure release, void* closure)
{
  struct KCPConversation* conversation;
  struct KCPSegment* segment;
  struct KCPKey key;
  uint32_t length;
  int result;
  int kind;

  if (format->verify((uint8_t*)packet, size) < 0)
  {
    // Malformed packet
    return -EINVAL;
  }

  memset(&key, 0, sizeof(struct KCPKey));

  switch (address->sa_family)
  {
    case AF_INET:   memcpy(&key.address, address, sizeof(struct sockaddr_in));  break;
    case AF_INET6:  memcpy(&key.address, address, sizeof(struct sockaddr_in6)); break;
  }

  if (time == NULL)
  {
    time = (struct timespec*)alloca(sizeof(struct timespec));
    clock_gettime(CLOCK_MONOTONIC, time);
  }

  if (reference != NULL)
  {
    // Reset conversation reference
    *reference = NULL;
  }

  conversation = NULL;

  while (size > 0)
  {
    if ((segment = AllocateKCPSegment(service)) == NULL)
    {
      // Ups...
      return -ENOMEM;
    }

    acquire(closure);

    length           = format->parse(&key, segment, (uint8_t*)packet, size);
    segment->release = release;
    segment->closure = closure;

    if (length == 0)
    {
      ReleaseKCPSegment(service, segment);
      break;
    }

    if (length > size)
    {
      ReleaseKCPSegment(service, segment);
      return -EPROTO;
    }

    if (((conversation == NULL) ||
         (memcmp(&key, &conversation->key, sizeof(struct KCPKey)) != 0)) &&
        (GetFromHashMap(service->map, &key, sizeof(struct KCPKey), (void**)&conversation) != HASHMAP_SUCCESS))
    {
      conversation = (struct KCPConversation*)calloc(1, sizeof(struct KCPConversation));

      if (conversation == NULL)
      {
        ReleaseKCPSegment(service, segment);
        return -ENOMEM;
      }

      memcpy(&conversation->key, &key, sizeof(struct KCPKey));

      if (PutIntoHashMap(service->map, &conversation->key, sizeof(struct KCPKey), conversation) != HASHMAP_SUCCESS)
      {
        ReleaseKCPSegment(service, segment);
        free(conversation);
        return -ENOMEM;
      }

      conversation->format  = format;
      conversation->service = service;
      conversation->timeout = KCP_DEFAULT_TIMEOUT;
      conversation->tries   = KCP_DEFAULT_TRIES;
      conversation->count   = 1;
      memcpy(&conversation->congestion, &service->congestion, sizeof(struct KCPCongestion));

      CallKCPHandler(service, conversation, KCP_EVENT_CREATE, NULL, 0);
    }

    if ((point                      != NULL)      &&
        (point->family              != AF_UNSPEC) &&
        (conversation->point.family == AF_UNSPEC))
    {
      // Copy local address to the conversation
      memcpy(&conversation->point, point, sizeof(struct KCPPoint));
    }

    if (conversation->format != format)
    {
      ReleaseKCPSegment(service, segment);
      return -EINVAL;
    }

    conversation->time.tv_sec  = time->tv_sec;
    conversation->time.tv_nsec = time->tv_nsec;

    // Proceed conversation state and go ahead

    if (reference != NULL)
    {
      // Pass conversation reference to the caller
      *reference = conversation;
    }

    if ((result = HandleIncomingKCPSegment(service, conversation, segment)) < 0)
    {
      // An error occurred while segment processed
      return result;
    }

    packet += length;
    size   -= length;
  }

  return 0;
}

// Transmitter

static int PrepareKCPSegmentList(struct KCPConversation* conversation, struct KCPSegment** list, uint32_t count, size_t length)
{
  const struct KCPFormat* format;
  struct KCPTransmitter* transmitter;
  struct KCPCongestion* congestion;
  struct KCPService* service;
  struct KCPSegment* segment;
  struct KCPSegment** entry;
  struct KCPQueue* queue;
  uint32_t amount;
  uint32_t size;

  if ((list  == NULL) ||
      (count == 0)    ||
      (count >  UINT8_MAX))
  {
    //
    return -EMSGSIZE;
  }

  service     = conversation->service;
  format      = conversation->format;
  queue       = &conversation->outbound;
  congestion  = &conversation->congestion;
  transmitter = service->transmitter;
  entry       = list;

  if ((count > (queue->size - queue->tail + queue->head)) &&
      (ExpandKCPQueue(queue, queue->size + count + UINT8_MAX) != 0))
  {
    // Cannot expand outbound queue
    return -ENOMEM;
  }

  while ((count != 0) &&
         (segment = AllocateKCPSegment(service)))
  {
    size    = (length < congestion->mss) ? length : congestion->mss;
    length -= size;

    amount           = format->propose((struct sockaddr*)&conversation->key.address, size);
    segment->closure = transmitter->allocate(transmitter->closure, amount);
    segment->release = transmitter->release;

    if (segment->closure == NULL)
    {
      ReleaseKCPSegment(service, segment);
      break;
    }

    format->prepare((uint8_t*)segment->closure, &conversation->key, segment, size);

    *entry  = segment;
    entry  ++;
    count  --;
  }

  if (count != 0)
  {
    while (entry != list)
    {
      segment = *(-- entry);
      ReleaseKCPSegment(service, segment);
    }

    return -ENOMEM;
  }

  return 0;
}

static void SubmitKCPSegment(struct KCPConversation* conversation, struct KCPSegment* segment, uint8_t frg)
{
  struct KCPControl* control;
  struct KCPQueue* queue;

  queue   = &conversation->outbound;
  control = segment->control;

  control->cmd = KCP_CMD_PUSH;
  control->frg = frg;

  queue->slots[(queue->tail ++) & (queue->size - 1)] = segment;
}

int SubmitKCPMessage(struct KCPConversation* conversation, const uint8_t* data, size_t length)
{
  struct KCPCongestion* congestion;
  struct KCPSegment* segment;
  struct KCPSegment** entry;
  uint32_t count;
  int result;

  if (conversation->state & KCP_CONVERSATION_DETACHED)  return -EBADF;
  if (conversation->state & KCP_CONVERSATION_DEAD)      return -ECONNRESET;

  congestion = &conversation->congestion;
  count      = (length / congestion->mss) + !!(length % congestion->mss) + (length == 0);
  entry      = (struct KCPSegment**)alloca(count * sizeof(struct KCPSegment*));
  result     = PrepareKCPSegmentList(conversation, entry, count, length);

  if (result == 0)
  {
    segment = *(entry ++);

    while (length > congestion->mss)
    {
      memcpy(segment->data, data, congestion->mss);
      SubmitKCPSegment(conversation, segment, -- count);
      data    += congestion->mss;
      length  -= congestion->mss;
      segment  = *(entry ++);
    }

    memcpy(segment->data, data, length);
    SubmitKCPSegment(conversation, segment, 0);
  }

  return result;
}

int SubmitKCPVectorList(struct KCPConversation* conversation, const struct iovec* list, size_t length)
{
  struct KCPCongestion* congestion;
  const struct iovec* vector;
  struct KCPSegment* segment;
  struct KCPSegment** entry;
  struct iovec destination;
  struct iovec source;
  uint32_t count;
  size_t amount;
  int result;

  if (conversation->state & KCP_CONVERSATION_DETACHED)  return -EBADF;
  if (conversation->state & KCP_CONVERSATION_DEAD)      return -ECONNRESET;

  vector = list;
  amount = 0;

  while (length != 0)
  {
    amount += vector->iov_len;
    vector ++;
    length --;
  }

  congestion = &conversation->congestion;
  count      = (amount / congestion->mss) + !!(amount % congestion->mss);
  entry      = (struct KCPSegment**)alloca(count * sizeof(struct KCPSegment*));
  result     = PrepareKCPSegmentList(conversation, entry, count, amount);

  if (result == 0)
  {
    vector               = list;
    segment              = *entry;
    source.iov_base      = vector->iov_base;
    source.iov_len       = vector->iov_len;
    destination.iov_base = segment->data;
    destination.iov_len  = segment->length;

    while (amount != 0)
    {
      length = (source.iov_len < destination.iov_len) ? source.iov_len : destination.iov_len;

      memcpy(destination.iov_base, source.iov_base, length);

      destination.iov_base  = (uint8_t*)destination.iov_base + length;
      source.iov_base       = (uint8_t*)source.iov_base      + length;
      destination.iov_len  -= length;
      source.iov_len       -= length;
      amount               -= length;

      if ((source.iov_len == 0) &&
          (amount         != 0))
      {
        vector          ++;
        source.iov_base  = vector->iov_base;
        source.iov_len   = vector->iov_len;
      }

      if (destination.iov_len == 0)
      {
        SubmitKCPSegment(conversation, segment, -- count);
        if (amount != 0)
        {
          segment              = *(++ entry);
          destination.iov_base = segment->data;
          destination.iov_len  = segment->length;
        }
      }
    }
  }

  return result;
}

int FlushKCPConversation(struct KCPConversation* conversation, struct timespec* time)
{
  const struct KCPFormat* format;
  struct KCPTransmitter* transmitter;
  struct KCPCongestion* congestion;
  struct KCPService* service;
  struct KCPSegment* segment;
  struct KCPControl* control;
  struct timespec threshold;
  struct KCPQueue* queue;
  uint32_t stamp;
  uint32_t count;
  uint32_t entry;
  uint32_t mask;
  uint16_t size;
  int result;

  result      = 0;
  service     = conversation->service;
  queue       = &conversation->outbound;
  congestion  = &conversation->congestion;
  format      = conversation->format;
  transmitter = service->transmitter;

  if (conversation->state & KCP_CONVERSATION_DETACHED)  return -EBADF;
  if (conversation->state & KCP_CONVERSATION_GUARD)     return -EBUSY;
  if (conversation->state & KCP_CONVERSATION_DEAD)      return -ECONNRESET;

  if (time == NULL)
  {
    time = (struct timespec*)alloca(sizeof(struct timespec));
    clock_gettime(CLOCK_MONOTONIC, time);
  }

  if (conversation->timeout > 0)
  {
    threshold.tv_sec   = conversation->time.tv_sec  + (conversation->timeout / 1000u);
    threshold.tv_nsec  = conversation->time.tv_nsec + (conversation->timeout % 1000u) * 1000000u;
    threshold.tv_sec  += threshold.tv_nsec / 1000000000u;
    threshold.tv_nsec %=                     1000000000u;

    if ((time->tv_sec  >  threshold.tv_sec) ||
        (time->tv_sec  == threshold.tv_sec) &&
        (time->tv_nsec >= threshold.tv_nsec))
    {
      conversation->state |= KCP_CONVERSATION_DEAD;
      conversation->cause  = -ETIME;
      return -ECONNRESET;
    }
  }

  stamp = time->tv_sec * 1000u + time->tv_nsec / 1000000u;

  if ((congestion->acknowledge.cmd == KCP_CMD_PUSH) ||
      (congestion->acknowledge.cmd == KCP_CMD_ACK)  &&
      ((int32_t)(stamp - congestion->ackdue) >= 0))
  {
    size   = GetKCPWindowSize(&conversation->inbound, congestion);
    result = TransmitKCPControlSegment(conversation, KCP_CMD_ACK, htole16(size), congestion->acknowledge.ts, congestion->acknowledge.sn, htole32(congestion->rcvnxt));
    congestion->acknowledge.cmd *= (result < 0);
  }

  if ((result      >= 0) &&
      (queue->head != queue->tail))
  {
    if (UpdateKCPZeroWindowProbe(congestion, stamp) != 0)
    {
      size   = GetKCPWindowSize(&conversation->inbound, congestion);
      result = TransmitKCPControlSegment(conversation, KCP_CMD_WASK, htole16(size), htole32(stamp), 0, htole32(congestion->rcvnxt));
    }

    count = GetKCPCongestionSendQuota(congestion, queue->tail - queue->head);
    entry = queue->head;
    mask  = queue->size - 1;

    while ((result >= 0) &&
           (entry  != queue->tail))
    {
      segment = queue->slots[(entry ++) & mask];
      control = segment->control;

      if (segment->tries >= conversation->tries)
      {
        conversation->state |= KCP_CONVERSATION_DEAD;
        conversation->cause  = -ECONNRESET;
        result               = -ECONNRESET;
        break;
      }

      if (segment->state & KCP_SEGMENT_SENT)
      {
        if ((int32_t)(stamp - le32toh(control->ts)) >= (int32_t)congestion->rto)
        {
          HandleKCPCongestionTimeout(congestion);
          segment->state &= ~KCP_SEGMENT_SENT;
          segment->track  = 0;
          count ++;
        }
        else if ((congestion->fastresend != 0) &&
                 (segment->track >= congestion->fastresend) &&
                 (segment->tries > 0))
        {
          HandleKCPCongestionFastResend(congestion);
          segment->state &= ~KCP_SEGMENT_SENT;
          segment->track  = 0;
          count ++;
        }
      }

      if ((count != 0) &&
          (~segment->state & KCP_SEGMENT_SENT))
      {
        control->ts  = htole32(stamp);
        control->wnd = htole16(GetKCPWindowSize(&conversation->inbound, congestion));
        control->una = htole32(congestion->rcvnxt);

        if (~segment->state & KCP_SEGMENT_NUMBERED)
        {
          control->sn     = htole32(congestion->control.sn ++);
          segment->state |= KCP_SEGMENT_NUMBERED;
        }

        format->compose(segment);

        result = transmitter->transmit(transmitter->closure, (struct sockaddr*)&conversation->key.address, segment->packet, segment->size);

        segment->state |= (result >= 0) * KCP_SEGMENT_SENT;
        segment->tries ++;
        count --;
      }

      if ((count == 0) &&
          (~segment->state & KCP_SEGMENT_SENT))
      {
        // Send quota exhausted. This segment is not in-flight, and the outbound queue is ordered as
        // an in-flight prefix followed by a not-yet-sent tail, so scanning further is pointless.
        break;
      }
    }
  }

  return result;
}

// Service operations

struct KCPConversationIterator
{
  int result;
  uint32_t flags;
  struct timespec time;
  struct KCPService* service;
};

static int HandleKCPConversation(void* key, size_t size, void* data, void* argument1, void* argument2)
{
  struct KCPConversationIterator* iterator;
  struct KCPConversation* conversation;
  int result;

  iterator     = (struct KCPConversationIterator*)argument1;
  conversation = (struct KCPConversation*)data;

  if ((iterator->flags & KCP_FLUSH_SEND) &&
      ((result = FlushKCPConversation(conversation, &iterator->time)) < 0))
  {
    // Store only fails
    iterator->result = result;
  }

  return (iterator->flags & KCP_FLUSH_CLEANUP) && (conversation->state & KCP_CONVERSATION_DEAD);
}

static void RemoveKCPConversation(void* key, void* data)
{
  struct KCPConversation* conversation;

  conversation         = (struct KCPConversation*)data;
  conversation->state |= KCP_CONVERSATION_DETACHED;

  if (~conversation->state & KCP_CONVERSATION_SILENT)
  {
    //
    CallKCPHandler(conversation->service, conversation, KCP_EVENT_REMOVE, NULL, 0);
  }

  ReleaseKCPConversation(conversation);
}

void ReleaseKCPConversation(struct KCPConversation* conversation)
{
  struct KCPService* service;
  struct KCPQueue* queue;
  uint32_t mask;

  if (conversation != NULL)
  {
    service              = conversation->service;
    conversation->count -= !!conversation->count;

    if (conversation->count <= 1)
    {
      if (conversation->state & KCP_CONVERSATION_GUARD)
      {
        conversation->state |= KCP_CONVERSATION_DEAD | KCP_CONVERSATION_SILENT;
        return;
      }

      if (~conversation->state & KCP_CONVERSATION_DETACHED)
      {
        conversation->state |= KCP_CONVERSATION_SILENT;
        RemoveFromHashMap(service->map, &conversation->key, sizeof(struct KCPKey), conversation);
        return;
      }
    }

    if (conversation->count == 0)
    {
      queue   = &conversation->outbound;
      mask    = queue->size - 1;

      while (queue->head != queue->tail)
      {
        ReleaseKCPSegment(service, queue->slots[queue->head & mask]);
        queue->head ++;
      }

      queue = &conversation->inbound;
      mask  = queue->size - 1;

      while (queue->head != queue->tail)
      {
        ReleaseKCPSegment(service, queue->slots[queue->head & mask]);
        queue->head ++;
      }

      free(conversation->outbound.slots);
      free(conversation->inbound.slots);
      free(conversation);
    }
  }
}

int CreateKCPConversation(struct KCPService* service, const struct KCPKey* key, const struct KCPFormat* format, struct KCPConversation** reference)
{
  struct KCPConversation* conversation;

  if (GetFromHashMap(service->map, key, sizeof(struct KCPKey), (void**)&conversation) == HASHMAP_SUCCESS)  return -EEXIST;
  if ((conversation = (struct KCPConversation*)calloc(1, sizeof(struct KCPConversation))) == NULL)         return -ENOMEM;

  memcpy(&conversation->key, key, sizeof(struct KCPKey));

  if (PutIntoHashMap(service->map, &conversation->key, sizeof(struct KCPKey), conversation) != HASHMAP_SUCCESS)
  {
    free(conversation);
    return -ENOMEM;
  }

  memcpy(&conversation->congestion, &service->congestion, sizeof(struct KCPCongestion));
  clock_gettime(CLOCK_MONOTONIC, &conversation->time);

  conversation->format  = format;
  conversation->service = service;
  conversation->timeout = KCP_DEFAULT_TIMEOUT;
  conversation->tries   = KCP_DEFAULT_TRIES;
  conversation->count   = 1;
  *reference            = conversation;

  return 0;
}

int FlushKCPService(struct KCPService* service, uint32_t flags)
{
  struct KCPConversationIterator iterator;

  iterator.result  = 0;
  iterator.flags   = flags;
  iterator.service = service;

  clock_gettime(CLOCK_MONOTONIC, &iterator.time);
  IterateThroughHashMap(service->map, HandleKCPConversation, &iterator, NULL);

  return iterator.result;
}

void ReleaseKCPService(struct KCPService* service)
{
  struct KCPQueue* queue;
  uint32_t mask;

  if (service != NULL)
  {
    ReleaseHashMap(service->map);

    queue = &service->cache;
    mask  = queue->size - 1;

    while (queue->head != queue->tail)
    {
      free(queue->slots[queue->head & mask]);
      queue->head ++;
    }

    free(service->cache.slots);
    free(service->buffer);
    free(service);
  }
}

struct KCPService* CreateKCPService(struct KCPHandler* handler, struct KCPTransmitter* transmitter)
{
  struct KCPService* service;

  if (service = (struct KCPService*)calloc(1, sizeof(struct KCPService)))
  {
    service->map         = CreateHashMap(RemoveKCPConversation);
    service->handler     = handler;
    service->transmitter = transmitter;
    InitializeKCPCongestion(&service->congestion);
  }

  return service;
}

// Standard KCP format description

static int VerifyStandardKCPPacket(uint8_t* packet, uint32_t size)
{
  struct StandardKCPHeader* header;

  header = (struct StandardKCPHeader*)packet;

  if (size >= sizeof(struct StandardKCPHeader))
  {
    size -= sizeof(struct StandardKCPHeader);
    if ((header->ctl.cmd == KCP_CMD_PUSH) &&
        (size >= le32toh(header->len))    ||
        (header->ctl.cmd >= KCP_CMD_ACK)  &&
        (header->ctl.cmd <= KCP_CMD_WINS) &&
        (header->len     == 0))
    {
      //
      return KCP_PACKET_EXACTLY_VALID;
    }
  }

  return KCP_PACKET_INVALID;
}

static uint32_t ParseStandardKCPPacket(struct KCPKey* key, struct KCPSegment* segment, uint8_t* packet, uint32_t size)
{
  struct StandardKCPHeader* header;

  if (VerifyStandardKCPPacket(packet, size) == KCP_PACKET_EXACTLY_VALID)
  {
    header = (struct StandardKCPHeader*)packet;

    key->conversation = header->conv;
    segment->control  = &header->ctl;
    segment->data     = packet + sizeof(struct StandardKCPHeader);
    segment->length   = le32toh(header->len);

    return sizeof(struct StandardKCPHeader) + segment->length;
  }

  return 0;
}

static uint32_t ProposeStandardKCPPacket(struct sockaddr* address, uint32_t length)
{
  return sizeof(struct StandardKCPHeader) + length;
}

static void PrepareStandardKCPPacket(uint8_t* buffer, struct KCPKey* key, struct KCPSegment* segment, uint32_t length)
{
  struct StandardKCPHeader* header;

  header       = (struct StandardKCPHeader*)buffer;
  header->conv = key->conversation;
  header->len  = htole32(length);

  segment->packet  = buffer;
  segment->size    = sizeof(struct StandardKCPHeader) + length;
  segment->control = &header->ctl;
  segment->data    = buffer + sizeof(struct StandardKCPHeader);
  segment->length  = length;
}

static void ComposeStandardKCPPacket(struct KCPSegment* segment)
{

}

const struct KCPFormat StandardKCPFormat =
{
  VerifyStandardKCPPacket,
  ParseStandardKCPPacket,
  ProposeStandardKCPPacket,
  PrepareStandardKCPPacket,
  ComposeStandardKCPPacket
};

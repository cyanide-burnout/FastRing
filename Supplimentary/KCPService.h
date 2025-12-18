#ifndef KCPSERVICE_H
#define KCPSERVICE_H

// https://blog.csdn.net/djh971102/article/details/132302609
// https://github.com/skywind3000/kcp

#include <time.h>
#include <stdint.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "HashMap.h"

#ifdef __cplusplus
extern "C"
{
#endif

// KCP message headers

#define KCP_CMD_PUSH  81  // data segment (PUSH)
#define KCP_CMD_ACK   82  // acknowledge for a received segment
#define KCP_CMD_WASK  83  // window size probe: ask peer to report its wnd
#define KCP_CMD_WINS  84  // window size report: peer reports its current wnd

struct KCPControl
{
  uint8_t cmd;   // KCP control opcode for this segment (DATA/PUSH, ACK, WASK, WINS, etc.)
  uint8_t frg;   // fragment index: 0 = last (or only) fragment, >0 = more fragments follow; used to reassemble a logical message
  uint16_t wnd;  // advertised receive window size (in segments): how many additional data segments this side is currently willing to accept
  uint32_t ts;   // sender's timestamp for this segment (monotonic tick, usually in ms): used by the peer to calculate RTT/RTO
  uint32_t sn;   // sequence number of this segment: unique per KCP stream, increases by 1 for each new data segment
  uint32_t una;  // "unacknowledged" lower bound: first segment *not yet acknowledged* from the sender's point of view; all sequence numbers < una are considered delivered and can be dropped
} __attribute__((packed));

struct StandardKCPHeader
{
  uint32_t conv;          // Connection number
  struct KCPControl ctl;  // Connection control
  uint32_t len;           // Length of data
} __attribute__((packed));

// KCP congestion control

#define KCP_DEFAULT_MSS         1024  // bytes
#define KCP_DEFAULT_RTO         200   // milliseconds
#define KCP_DEFAULT_WND         128   // segments
#define KCP_DEFAULT_INTERVAL    100   // milliseconds
#define KCP_DEFAULT_ACKTHRESH   16    // segments
#define KCP_DEFAULT_FASTRESEND  2     // 0 / 2

struct KCPCongestion
{
  struct KCPControl control;      // counters: wnd = local receive window capacity (in segments), sn = next sequence to send, una = lowest unacked sequence
  struct KCPControl acknowledge;  // acknowledge parameters: cmd = KCP_CMD_ACK/KCP_CMD_PUSH - armed, sn = sequence number, ts = timestamp

  uint32_t mss;                   // maximum segment size in bytes
  uint32_t cwnd;                  // congestion window (in segments)
  uint32_t ssthresh;              // slow start threshold (in segments)
  uint32_t incr;                  // accumulator used to grow cwnd (in bytes)
  uint32_t rmtwnd;                // last advertised window from peer (wnd field from incoming segments)
  uint32_t fastresend;            // fastack threshold for fast retransmit (0 = disabled)
  uint8_t nocwnd;                 // if nonzero, ignore cwnd and use only rmtwnd
  uint32_t rcvnxt;                // host-order, next expected sn on RX

  uint32_t srtt;                  // smoothed RTT
  uint32_t rttvar;                // RTT variance
  uint32_t rto;                   // current retransmit timeout
  uint32_t interval;              // base timer interval (tick), milliseconds

  uint32_t pwait;                 // delay between window probes in milliseconds
  uint32_t pts;                   // next scheduled probe time in milliseconds

  uint32_t ackdue;                // acknowledge's due (in milliseconds)
  uint16_t ackthresh;             // acknowledge's threshold (in segments)
};

// KCP format description

#define KCP_PACKET_INVALID         -1
#define KCP_PACKET_PROBABLY_VALID   0
#define KCP_PACKET_EXACTLY_VALID    1

struct KCPKey;
struct KCPSegment;

typedef int (*VerifyKCPPacket)(uint8_t* packet, uint32_t size);
typedef void (*ParseKCPPacket)(struct KCPKey* key, struct KCPSegment* segment, uint8_t* packet, uint32_t size);
typedef uint32_t (*ProposeKCPPacket)(struct sockaddr* address, uint32_t length);
typedef void (*PrepareKCPPacket)(uint8_t* buffer, struct KCPKey* key, struct KCPSegment* segment, uint32_t length);
typedef void (*ComposeKCPPacket)(struct KCPSegment* segment);

struct KCPFormat
{
  VerifyKCPPacket verify;
  ParseKCPPacket parse;
  ProposeKCPPacket propose;
  PrepareKCPPacket prepare;
  ComposeKCPPacket compose;
};

// KCP service

#define KCP_DEFAULT_TIMEOUT  90000  // milliseconds
#define KCP_DEFAULT_TRIES    8

#define KCP_FLUSH_CLEANUP  (1 << 0)
#define KCP_FLUSH_SEND     (1 << 1)

#define KCP_SEGMENT_NUMBERED  (1 << 0)
#define KCP_SEGMENT_ANCHOR    (1 << 1)
#define KCP_SEGMENT_SENT      (1 << 2)

#define KCP_EVENT_CREATE   0
#define KCP_EVENT_REMOVE   1
#define KCP_EVENT_RECEIVE  2

#define KCP_CONVERSATION_DETACHED  (1 << 0)
#define KCP_CONVERSATION_SILENT    (1 << 1)
#define KCP_CONVERSATION_GUARD     (1 << 2)
#define KCP_CONVERSATION_DEAD      (1 << 3)

struct KCPService;
struct KCPConversation;

typedef void (*HandleKCPEvent)(void* closure, struct KCPConversation* conversation, int event, uint8_t* data, size_t length);

typedef uint8_t* (*AllocateKCPPacket)(void* closure, uint32_t size);
typedef int (*TransmitKCPPacket)(void* closure, struct sockaddr* address, uint8_t* data, uint32_t size);

typedef void (*ReleaseKCPClosure)(void* closure);

union KCPAddress
{
  sa_family_t family;
  struct sockaddr_in v4;
  struct sockaddr_in6 v6;
};

struct KCPPoint
{
  sa_family_t family;
  union
  {
    uint8_t any[0];
    struct in_addr v4;
    struct in6_addr v6;
  };
};

struct KCPKey
{
  uint64_t application;
  uint64_t conversation;
  union KCPAddress address;
};

struct KCPSegment
{
  uint32_t state;              // KCP_SEGMENT_*
  uint32_t tries;              // Count of tries
  uint32_t track;              // Fast ACK count

  struct KCPControl* control;  // KCP control
  uint8_t* data;               // Segment payload
  uint32_t length;             // Payload length

  uint8_t* packet;             // Packet buffer
  uint32_t size;               // Packet size
  void* closure;               //
  ReleaseKCPClosure release;   //
};

struct KCPQueue
{
  uint32_t head;
  uint32_t tail;
  uint32_t size;
  struct KCPSegment** slots;
};

struct KCPConversation
{
  uint32_t state;
  void* closure;
  int timeout;
  int tries;
  int count;
  int cause;

  struct KCPKey key;
  struct timespec time;
  struct KCPPoint point;
  struct KCPQueue inbound;
  struct KCPQueue outbound;
  struct KCPCongestion congestion;

  struct KCPService* service;
  const struct KCPFormat* format;
};

struct KCPHandler
{
  void* closure;
  HandleKCPEvent handle;
};

struct KCPTransmitter
{
  void* closure;
  AllocateKCPPacket allocate;
  TransmitKCPPacket transmit;
  ReleaseKCPClosure release;
};

struct KCPService
{
  struct HashMap* map;
  struct KCPTransmitter* transmitter;
  struct KCPHandler* handler;
  struct KCPQueue cache;
  uint8_t* buffer;
  size_t size;

  struct KCPCongestion congestion;
};

int HandleKCPPacket(struct KCPService* service, const struct KCPFormat* format, struct KCPConversation** reference, struct timespec* time, struct sockaddr* address, void* packet, uint32_t size, struct KCPPoint* point, ReleaseKCPClosure release, void* closure);
int SubmitKCPMessage(struct KCPConversation* conversation, const uint8_t* data, size_t length);
int SubmitKCPVectorList(struct KCPConversation* conversation, const struct iovec* list, size_t length);
int FlushKCPConversation(struct KCPConversation* conversation, struct timespec* time);

void ReleaseKCPConversation(struct KCPConversation* conversation);
int CreateKCPConversation(struct KCPService* service, const struct KCPKey* key, const struct KCPFormat* format, struct KCPConversation** reference);

int FlushKCPService(struct KCPService* service, uint32_t flags);
void ReleaseKCPService(struct KCPService* service);
struct KCPService* CreateKCPService(struct KCPHandler* handler, struct KCPTransmitter* transmitter);

extern const struct KCPFormat StandardKCPFormat;

#ifdef __cplusplus
}
#endif

#endif

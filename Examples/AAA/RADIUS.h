#ifndef RADIUS_H
#define RADIUS_H

// RFC2865, RFC2866, RFC3162, RFC6911

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define RADIUS_CODE_ACCT_REQ           4
#define RADIUS_CODE_ACCT_RESP          5

#define RADIUS_TYPE_FRAMED_IP          8    // Framed-IP-Address
#define RADIUS_TYPE_FRAMED_IP6         168  // Framed-IPv6-Address
#define RADIUS_TYPE_FRAMED_IP6_PREFIX  97   // Framed-IPv6-Prefix
#define RADIUS_TYPE_STATION_ID         31   // Calling-Station-Id

#define RADIUS_TYPE_STATUS             40   // Acct-Status-Type
#define RADIUS_TYPE_INPUT_BYTES        42   // Acct-Input-Octets
#define RADIUS_TYPE_OUTPUT_BYTES       43   // Acct-Output-Octets
#define RADIUS_TYPE_SESSION_ID         44   // Acct-Session-Id
#define RADIUS_TYPE_SESSION_TIME       46   // Acct-Session-Time
#define RADIUS_TYPE_INPUT_PACKTES      47   // Acct-Input-Packets
#define RADIUS_TYPE_OUTPUT_PACKTES     48   // Acct-Output-Packets
#define RADIUS_TYPE_TERMINATE_CAUSE    49   // Acct-Terminate-Cause

#define RADIUS_STATUS_START            1
#define RADIUS_STATUS_STOP             2
#define RADIUS_STATUS_UPDATE           3

struct RADIUSAttribute
{
  uint8_t type;
  uint8_t length;
  union
  {
    uint8_t data[0];
    uint32_t value;
  };
} __attribute__((packed));

struct RADIUSDataUnit
{
  uint8_t code;
  uint8_t identifier;
  uint16_t length;
  uint8_t authenticator[16];
  uint8_t data[0];
} __attribute__((packed));

#ifdef __cplusplus
}
#endif

#endif

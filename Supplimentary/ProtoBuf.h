#ifndef PROTOBUF_H
#define PROTOBUF_H

#include <stddef.h>
#include <stdint.h>
#include <alloca.h>
#include <protobuf-c/protobuf-c.h>

#ifdef __cplusplus
extern "C"
{
#endif

struct ProtoBufArena
{
  ProtobufCAllocator table;
  size_t allingment;
  size_t length;
  uint8_t* pointer;
  uint8_t data[0];
};

ProtobufCAllocator* InitializeProtoBufArena(void* buffer, size_t length);

#define CreateProtoBufArena(length)  InitializeProtoBufArena(alloca(sizeof(struct ProtoBufArena) + length), length)

#ifdef __cplusplus
}
#endif

#endif

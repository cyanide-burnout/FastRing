#include "Sombrero.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if __has_include(<sha2.h>)
#include <sha2.h>
#elif __has_include(<openssl/sha.h>)
#include <openssl/sha.h>
#endif

#if __has_include(<openssl/hmac.h>)
#include <openssl/hmac.h>
#endif

#include <smb2/libsmb2-raw.h>

#if __has_include(<libsmb2-private.h>)
#include <libsmb2-private.h>
#elif __has_include(<smb2/libsmb2-private.h>)
#include <smb2/libsmb2-private.h>
#else

#if __has_include(<gssapi/gssapi.h>)
#include <gssapi/gssapi.h>
#define SOMBRERO_HAVE_LIBKRB5  1
#elif __APPLE__ && __has_include(<GSS/GSS.h>)
#import <GSS/GSS.h>
#define SOMBRERO_HAVE_LIBKRB5  1
#endif

#ifndef SMB2_HEADER_SIZE
#define SMB2_HEADER_SIZE  64
#endif

#ifndef SMB2_KEY_SIZE
#define SMB2_KEY_SIZE  16
#endif

#ifndef SMB2_SALT_SIZE
#define SMB2_SALT_SIZE  32
#endif

#ifndef SMB2_MAX_VECTORS
#define SMB2_MAX_VECTORS  256
#endif

#ifndef SMB2_CREATE_ACTION_OPENED
#define SMB2_CREATE_ACTION_OPENED  0x00000001U
#endif

#ifndef SMB2_PREAUTH_HASH_SIZE
#define SMB2_PREAUTH_HASH_SIZE  64
#endif

#define MAX_ERROR_SIZE          256
#define SMB2_MAX_TREE_NESTING   32

struct smb2_io_vectors
{
  size_t num_done;
  size_t total_size;
  int niov;
  struct smb2_iovec iov[SMB2_MAX_VECTORS];
};

struct smb2_async
{
  uint64_t async_id;
};

struct smb2_sync
{
  uint32_t process_id;
  uint32_t tree_id;
};

struct smb2_header
{
  uint8_t protocol_id[4];
  uint16_t struct_size;
  uint16_t credit_charge;
  uint32_t status;
  uint16_t command;
  uint16_t credit_request_response;
  uint32_t flags;
  uint32_t next_command;
  uint64_t message_id;
  union
  {
    struct smb2_async async;
    struct smb2_sync sync;
  };
  uint64_t session_id;
  uint8_t signature[16];
};

enum smb2_recv_state
{
  SMB2_RECV_SPL = 0,
  SMB2_RECV_HEADER,
  SMB2_RECV_FIXED,
  SMB2_RECV_VARIABLE,
  SMB2_RECV_PAD,
  SMB2_RECV_TRFM
};

enum smb2_sec
{
  SMB2_SEC_UNDEFINED = 0,
  SMB2_SEC_NTLMSSP,
  SMB2_SEC_KRB5
};

struct sync_cb_data
{
  int is_finished;
  int status;
  void* ptr;
};

struct smb2_pdu
{
  struct smb2_pdu* next;
  struct smb2_header header;
  struct smb2_pdu* next_compound;
  smb2_command_cb cb;
  void* cb_data;
  uint8_t hdr[SMB2_HEADER_SIZE];
  void* payload;
  void (*free_payload)(struct smb2_context* smb2, void* payload);
  struct smb2_io_vectors out;
  struct smb2_io_vectors in;
  uint8_t info_type;
  uint8_t file_info_class;
  uint8_t seal:1;
  uint32_t crypt_len;
  unsigned char* crypt;
  time_t timeout;
};

struct smb2_context
{
  t_socket fd;
  struct smb2_server* owning_server;
  t_socket* connecting_fds;
  size_t connecting_fds_count;
  struct addrinfo* addrinfos;
  const struct addrinfo* next_addrinfo;
  int timeout;
  enum smb2_sec sec;
  uint16_t security_mode;
  uint32_t capabilities;
  int use_cached_creds;
  enum smb2_negotiate_version version;
  const char* server;
  const char* share;
  const char* user;
  const char* password;
  const char* domain;
  const char* workstation;
  char client_challenge[8];
  void* opaque;
  smb2_error_cb error_cb;
  smb2_command_cb connect_cb;
  void* connect_data;
  struct sync_cb_data connect_cb_data;
  int credits;
  char client_guid[16];
  uint32_t tree_id[SMB2_MAX_TREE_NESTING];
  int tree_id_top;
  int tree_id_cur;
  uint64_t message_id;
  uint64_t session_id;
  uint8_t* session_key;
  uint8_t session_key_size;
  uint8_t seal:1;
  uint8_t sign:1;
  uint8_t signing_key[SMB2_KEY_SIZE];
  uint8_t serverin_key[SMB2_KEY_SIZE];
  uint8_t serverout_key[SMB2_KEY_SIZE];
  uint8_t salt[SMB2_SALT_SIZE];
  uint16_t cypher;
  uint8_t preauthhash[SMB2_PREAUTH_HASH_SIZE];

#if SOMBRERO_HAVE_LIBKRB5
  gss_cred_id_t cred_handle;
#endif

  unsigned char* enc;
  size_t enc_len;
  int enc_pos;
  struct smb2_pdu* outqueue;
  struct smb2_pdu* waitqueue;
  struct smb2_io_vectors in;
  enum smb2_recv_state recv_state;
  uint32_t spl;
  uint8_t header[SMB2_HEADER_SIZE];
  struct smb2_header hdr;
  size_t payload_offset;
  struct smb2_pdu* pdu;
  struct smb2_pdu* next_pdu;
  int passthrough;
  smb2_oplock_or_lease_break_cb oplock_or_lease_break_cb;
  int oplock_break_count;
  smb2_file_id last_file_id;
  uint8_t supports_multi_credit;
  uint32_t max_transact_size;
  uint32_t max_read_size;
  uint32_t max_write_size;
  uint16_t dialect;
  char error_string[MAX_ERROR_SIZE];
  int nterror;
  struct smb2fh* fhs;
  struct smb2dir* dirs;
  int events;
  smb2_change_fd_cb change_fd;
  smb2_change_events_cb change_events;
  uint8_t ndr;
  int endianness;
  struct smb2_context* next;
};

struct connect_data;

struct smb2_pdu* smb2_allocate_pdu(struct smb2_context* smb2, enum smb2_command command, smb2_command_cb cb, void* cb_data);
int smb2_disconnect_tree_id(struct smb2_context* smb2, uint32_t tree_id);

#endif

#define NEGOTIATE_MESSAGE      1U
#define AUTHENTICATION_MESSAGE 3U
#define SOMBRERO_PIPE_NAME     "echo"
#define SOMBRERO_SHARE_NAME    "IPC$"
#define SOMBRERO_ROOT_ACCESS   0x101f01ffU
#define PAD_TO_64BIT(length)   (((length) + 0x07U) & 0xfffffff8U)
#define FS_TOTAL_UNITS         0x100000ULL
#define FS_AVAILABLE_UNITS     0x080000ULL
#define FS_SECTORS_PER_UNIT    8U
#define FS_BYTES_PER_SECTOR    512U

struct connect_data
{
  smb2_command_cb cb;
  void* cb_data;
  const char* server;
  const char* share;
  const char* user;
  char* utf8_unc;
  struct smb2_utf16* utf16_unc;
  void* auth_data;
  struct smb2_server* server_context;
};

struct SombreroPipeHandle
{
  struct SombreroPipeHandle* next;
  smb2_file_id file_id;
  uint8_t* buffer;
  uint32_t length;
  uint8_t kind;
};

struct SombreroSession
{
  struct SambarOpaque sambar;
  struct SombreroPipeHandle* handles;
  uint64_t next_handle_id;
  uint8_t tree_connected;
};

enum SombreroNodeType
{
  SOMBRERO_NODE_NONE = 0,
  SOMBRERO_NODE_ROOT = 1,
  SOMBRERO_NODE_PIPE_DIRECTORY = 2,
  SOMBRERO_NODE_PIPE = 3
};

extern void smb2_queue_pdu(struct smb2_context* smb2, struct smb2_pdu* pdu);
extern void smb2_free_pdu(struct smb2_context* smb2, struct smb2_pdu* pdu);
extern int smb2_spnego_create_negotiate_reply_blob(struct smb2_context* smb2, void** buffer);

extern void* ntlmssp_init_context(const char* user, const char* password, const char* domain, const char* workstation, char* client_challenge);
extern void ntlmssp_destroy_context(void* context);
extern int ntlmssp_generate_blob(struct smb2_server* server, struct smb2_context* smb2, time_t timestamp, void* auth_data, uint8_t* input, uint16_t length, uint8_t** output, uint16_t* output_length);
extern int ntlmssp_get_message_type(struct smb2_context* smb2, uint8_t* buffer, uint16_t length, uint32_t* message_type, uint8_t** response_token, int* response_length, int* is_spnego_wrapped);
extern int ntlmssp_get_authenticated(void* auth_data);
extern int ntlmssp_get_session_key(void* auth_data, uint8_t** session_key, uint8_t* session_key_size);

static int HandleDestroyPipeServer(struct smb2_server* server, struct smb2_context* context);
static int HandleAuthorizePipeServer(struct smb2_server* server, struct smb2_context* context, const char* user, const char* domain, const char* workstation);
static int HandlePipeSessionEstablished(struct smb2_server* server, struct smb2_context* context);
static int HandlePipeTreeConnect(struct smb2_server* server, struct smb2_context* context, struct smb2_tree_connect_request* request, struct smb2_tree_connect_reply* reply);
static int HandlePipeTreeDisconnect(struct smb2_server* server, struct smb2_context* context, uint32_t tree_id);
static int HandlePipeCreate(struct smb2_server* server, struct smb2_context* context, struct smb2_create_request* request, struct smb2_create_reply* reply);
static int HandlePipeClose(struct smb2_server* server, struct smb2_context* context, struct smb2_close_request* request, struct smb2_close_reply* reply);
static int HandlePipeFlush(struct smb2_server* server, struct smb2_context* context, struct smb2_flush_request* request);
static int HandlePipeRead(struct smb2_server* server, struct smb2_context* context, struct smb2_read_request* request, struct smb2_read_reply* reply);
static int HandlePipeWrite(struct smb2_server* server, struct smb2_context* context, struct smb2_write_request* request, struct smb2_write_reply* reply);
static int HandlePipeLock(struct smb2_server* server, struct smb2_context* context, struct smb2_lock_request* request);
static int HandlePipeIoctl(struct smb2_server* server, struct smb2_context* context, struct smb2_ioctl_request* request, struct smb2_ioctl_reply* reply);
static int HandlePipeCancel(struct smb2_server* server, struct smb2_context* context);
static int HandlePipeEcho(struct smb2_server* server, struct smb2_context* context);
static int HandlePipeQueryInfo(struct smb2_server* server, struct smb2_context* context, struct smb2_query_info_request* request, struct smb2_query_info_reply* reply);
static void HandleSombreroError(struct smb2_context* context, const char* error);

static void smb3_init_preauth_hash(struct smb2_context* context)
{
  memset(&context->preauthhash[0], 0, SMB2_PREAUTH_HASH_SIZE);
}

static int smb3_update_preauth_hash(struct smb2_context* context, int niov, struct smb2_iovec* iov)
{
  int index;

#if __has_include(<sha2.h>)
  SHA2_CTX state;

  SHA512Init(&state);
  SHA512Update(&state, context->preauthhash, SMB2_PREAUTH_HASH_SIZE);

  for (index = 0; index < niov; index++)
  {
    SHA512Update(&state, iov[index].buf, iov[index].len);
  }

  SHA512Final(context->preauthhash, &state);
  return 0;
#elif __has_include(<openssl/sha.h>)
  SHA512_CTX state;

  SHA512_Init(&state);
  SHA512_Update(&state, context->preauthhash, SMB2_PREAUTH_HASH_SIZE);

  for (index = 0; index < niov; index++)
  {
    SHA512_Update(&state, iov[index].buf, iov[index].len);
  }

  SHA512_Final(context->preauthhash, &state);
  return 0;
#else
  if ((context == NULL) && (niov == 0) && (iov == NULL))
  {
  }

  return -1;
#endif
}

static void DeriveKey(const uint8_t* derivation_key, uint32_t derivation_key_length, const char* label, uint32_t label_length, const char* context_value, uint32_t context_length, uint8_t derived_key[SMB2_KEY_SIZE])
{
  static const unsigned char counter[]    = { 0x00, 0x00, 0x00, 0x01 };
  static const unsigned char key_length[] = { 0x00, 0x00, 0x00, SMB2_KEY_SIZE * 8U };
  static const unsigned char zero = 0;
  unsigned char input[128];
  uint8_t input_key[SMB2_KEY_SIZE];
  size_t input_length;

  memset(input_key, 0, sizeof(input_key));
  memcpy(input_key, derivation_key, derivation_key_length < sizeof(input_key) ? derivation_key_length : sizeof(input_key));

#if __has_include(<openssl/hmac.h>)
  {
    unsigned int digest_length;
    uint8_t digest[32];

    if ((sizeof(counter) + label_length + 1U + context_length + sizeof(key_length)) > sizeof(input))  goto zero_key;

    input_length = 0;
    memcpy(&input[input_length], counter, sizeof(counter));
    input_length += sizeof(counter);
    memcpy(&input[input_length], label, label_length);
    input_length += label_length;
    input[input_length++] = zero;
    memcpy(&input[input_length], context_value, context_length);
    input_length += context_length;
    memcpy(&input[input_length], key_length, sizeof(key_length));
    input_length += sizeof(key_length);

    if (HMAC(EVP_sha256(), input_key, sizeof(input_key), input, input_length, digest, &digest_length) == NULL)  goto zero_key;

    memcpy(derived_key, digest, SMB2_KEY_SIZE);
    return;
  }
#endif

zero_key:
  memset(derived_key, 0, SMB2_KEY_SIZE);
}

static void CreateSigningKey(struct smb2_context* context)
{
  static const char smb2_aes_cmac[] = "SMB2AESCMAC";
  static const char smb2_aes_ccm[]  = "SMB2AESCCM";
  static const char smb_sign[]      = "SmbSign";
  static const char server_in[]     = "ServerIn ";
  static const char server_out[]    = "ServerOut";

  if ((context == NULL) || (context->session_key == NULL) || (context->session_key_size == 0))
    return;

  if ((context->dialect == SMB2_VERSION_0202) ||
      (context->dialect == SMB2_VERSION_0210))
  {
    memcpy(context->signing_key, context->session_key, context->session_key_size < SMB2_KEY_SIZE ? context->session_key_size : SMB2_KEY_SIZE);
    return;
  }

  if (context->dialect <= SMB2_VERSION_0302)
  {
    DeriveKey(context->session_key, context->session_key_size, smb2_aes_cmac, (uint32_t)sizeof(smb2_aes_cmac), smb_sign,   (uint32_t)sizeof(smb_sign),   context->signing_key);
    DeriveKey(context->session_key, context->session_key_size, smb2_aes_ccm,  (uint32_t)sizeof(smb2_aes_ccm),  server_in,  (uint32_t)sizeof(server_in),  context->serverin_key);
    DeriveKey(context->session_key, context->session_key_size, smb2_aes_ccm,  (uint32_t)sizeof(smb2_aes_ccm),  server_out, (uint32_t)sizeof(server_out), context->serverout_key);
    return;
  }

  DeriveKey(context->session_key, context->session_key_size, smb_sign,   (uint32_t)sizeof(smb_sign),   context->preauthhash, SMB2_PREAUTH_HASH_SIZE, context->signing_key);
  DeriveKey(context->session_key, context->session_key_size, server_in,  (uint32_t)sizeof(server_in),  context->preauthhash, SMB2_PREAUTH_HASH_SIZE, context->serverin_key);
  DeriveKey(context->session_key, context->session_key_size, server_out, (uint32_t)sizeof(server_out), context->preauthhash, SMB2_PREAUTH_HASH_SIZE, context->serverout_key);
}

static struct SombreroSession* GetSombreroSession(struct smb2_context* context)
{
  return context != NULL ? GetSambarData(context, struct SombreroSession, sambar) : NULL;
}

static void FreeSombreroHandles(struct SombreroSession* session)
{
  struct SombreroPipeHandle* handle;
  struct SombreroPipeHandle* next;

  for (handle = session != NULL ? session->handles : NULL; handle != NULL; handle = next)
  {
    next = handle->next;
    free(handle->buffer);
    free(handle);
  }

  if (session != NULL)
    session->handles = NULL;
}

static struct SombreroPipeHandle* FindSombreroHandle(struct SombreroSession* session, const smb2_file_id file_id)
{
  struct SombreroPipeHandle* handle;

  for (handle = session != NULL ? session->handles : NULL; handle != NULL; handle = handle->next)
    if (memcmp(handle->file_id, file_id, SMB2_FD_SIZE) == 0)
      return handle;

  return NULL;
}

static int IsZeroFileId(const smb2_file_id file_id)
{
  static const uint8_t zero_file_id[SMB2_FD_SIZE] = { 0 };

  return memcmp(file_id, zero_file_id, sizeof(zero_file_id)) == 0;
}

static int SetSombreroHandlePayload(struct SombreroPipeHandle* handle, const uint8_t* data, uint32_t length)
{
  uint8_t* buffer;

  if (handle == NULL)
    return -EINVAL;

  buffer = NULL;

  if ((length > 0U) &&
      ((buffer = (uint8_t*)malloc(length)) == NULL))
    return -ENOMEM;

  if ((buffer != NULL) &&
      (data   != NULL))
    memcpy(buffer, data, length);

  free(handle->buffer);
  handle->buffer = buffer;
  handle->length = length;
  return 0;
}

static void RemoveSombreroHandle(struct SombreroSession* session, struct SombreroPipeHandle* target)
{
  struct SombreroPipeHandle** cursor;

  if ((session == NULL) ||
      (target  == NULL))
    return;

  for (cursor = &session->handles; *cursor != NULL; cursor = &(*cursor)->next)
  {
    if (*cursor == target)
    {
      *cursor = target->next;
      free(target->buffer);
      free(target);
      return;
    }
  }
}

static int MatchTerminalComponent(const char* path, size_t units, const char* name)
{
  size_t offset;
  size_t index;
  size_t length;

  if ((path == NULL) ||
      (name == NULL))
    return 0;

  length = strlen(name);
  offset = units;

  while ((offset > 0U) &&
         (path[offset - 1U] != '\\') &&
         (path[offset - 1U] != '/'))
    offset --;

  if ((units - offset) != length)
    return 0;

  for (index = 0; index < length; index++)
    if (tolower((unsigned char)path[offset + index]) != tolower((unsigned char)name[index]))
      return 0;

  return 1;
}

static int MatchTerminalComponentUTF16(const uint16_t* path, size_t units, const char* name)
{
  size_t offset;
  size_t index;
  size_t length;

  if ((path == NULL) ||
      (name == NULL))
    return 0;

  length = strlen(name);
  offset = units;

  while ((offset > 0U) &&
         (path[offset - 1U] != '\\') &&
         (path[offset - 1U] != '/'))
    offset --;

  if ((units - offset) != length)
    return 0;

  for (index = 0; index < length; index++)
    if (tolower((unsigned char)path[offset + index]) != tolower((unsigned char)name[index]))
      return 0;

  return 1;
}

static int MatchNamedPipePath(const char* name)
{
  static const char pipe_prefix[] = "pipe";
  size_t index;

  if (name == NULL)
    return 0;

  while ((*name == '\\') || (*name == '/'))
    name ++;

  for (index = 0; index < (sizeof(pipe_prefix) - 1U); index++)
    if (tolower((unsigned char)name[index]) != pipe_prefix[index])
      return MatchTerminalComponent(name, strlen(name), SOMBRERO_PIPE_NAME);

  if ((name[index] == '\\') || (name[index] == '/'))
    name += index + 1U;

  while ((*name == '\\') || (*name == '/'))
    name ++;

  return strcasecmp(name, SOMBRERO_PIPE_NAME) == 0;
}

static int MatchPipeDirectoryPath(const char* name)
{
  if (name == NULL)
    return 0;

  while ((*name == '\\') || (*name == '/'))
    name ++;

  return strcasecmp(name, "pipe") == 0;
}

static int MatchIpcRootPath(const char* name)
{
  if (name == NULL)
    return 1;

  while ((*name == '\\') || (*name == '/'))
    name ++;

  return *name == '\0';
}

static enum SombreroNodeType GetNodeByHandle(const struct SombreroPipeHandle* handle)
{
  if (handle == NULL)
    return SOMBRERO_NODE_NONE;

  return (enum SombreroNodeType)handle->kind;
}

static uint64_t GetNodeLength(enum SombreroNodeType node, const struct SombreroPipeHandle* handle)
{
  return node == SOMBRERO_NODE_PIPE && handle != NULL ? handle->length : 0U;
}

static uint64_t GetNodeAllocationSize(enum SombreroNodeType node, const struct SombreroPipeHandle* handle)
{
  return node == SOMBRERO_NODE_PIPE ? GetNodeLength(node, handle) : 4096U;
}

static uint32_t GetNodeAttributes(enum SombreroNodeType node)
{
  return ((node == SOMBRERO_NODE_ROOT) || (node == SOMBRERO_NODE_PIPE_DIRECTORY))
           ? SMB2_FILE_ATTRIBUTE_DIRECTORY
           : SMB2_FILE_ATTRIBUTE_NORMAL;
}

static uint64_t GetNodeIndexNumber(enum SombreroNodeType node)
{
  switch (node)
  {
    case SOMBRERO_NODE_ROOT:            return 1U;
    case SOMBRERO_NODE_PIPE_DIRECTORY:  return 2U;
    case SOMBRERO_NODE_PIPE:            return 3U;
  }

  return 0U;
}

static void FillCurrentTimeval(struct smb2_timeval* value)
{
  if (value != NULL)
  {
    value->tv_sec  = time(NULL);
    value->tv_usec = 0;
  }
}

static void FillSombreroPipeFileId(struct SombreroSession* session, smb2_file_id file_id)
{
  uint64_t value;

  memset(file_id, 0, SMB2_FD_SIZE);
  value = session->next_handle_id ++;
  memcpy(&file_id[0], &value, sizeof(value));
  file_id[ 8] = 'P';
  file_id[ 9] = 'I';
  file_id[10] = 'P';
  file_id[11] = 'E';
}

static void FillPipeCreateReply(struct smb2_create_reply* reply, const smb2_file_id file_id)
{
  memset(reply, 0, sizeof(struct smb2_create_reply));
  memcpy(reply->file_id, file_id, SMB2_FD_SIZE);
  reply->create_action   = SMB2_CREATE_ACTION_OPENED;
  reply->oplock_level    = SMB2_OPLOCK_LEVEL_NONE;
  reply->file_attributes = SMB2_FILE_ATTRIBUTE_NORMAL;
}

static void FillRootCreateReply(struct smb2_create_reply* reply, const smb2_file_id file_id)
{
  memset(reply, 0, sizeof(struct smb2_create_reply));
  memcpy(reply->file_id, file_id, SMB2_FD_SIZE);
  reply->create_action   = SMB2_CREATE_ACTION_OPENED;
  reply->oplock_level    = SMB2_OPLOCK_LEVEL_NONE;
  reply->file_attributes = SMB2_FILE_ATTRIBUTE_DIRECTORY;
}

static void FillPipeDirectoryCreateReply(struct smb2_create_reply* reply, const smb2_file_id file_id)
{
  FillRootCreateReply(reply, file_id);
}

static void FillPipeCloseReply(const struct SombreroPipeHandle* handle, struct smb2_close_reply* reply)
{
  enum SombreroNodeType node;

  node = GetNodeByHandle(handle);
  memset(reply, 0, sizeof(struct smb2_close_reply));
  reply->allocation_size = GetNodeLength(node, handle);
  reply->end_of_file     = GetNodeLength(node, handle);
  reply->file_attributes = GetNodeAttributes(node);
}

static int FillFileSystemSizeInfoRaw(uint8_t** output)
{
  uint8_t* buffer;
  uint64_t units;
  uint64_t available_units;
  uint32_t sectors_per_unit;
  uint32_t bytes_per_sector;

  if ((buffer = (uint8_t*)calloc(1, 24U)) == NULL)
  {
    return -ENOMEM;
  }

  units            = FS_TOTAL_UNITS;
  available_units  = FS_AVAILABLE_UNITS;
  sectors_per_unit = FS_SECTORS_PER_UNIT;
  bytes_per_sector = FS_BYTES_PER_SECTOR;

  memcpy(&buffer[0],  &units,            sizeof(units));
  memcpy(&buffer[8],  &available_units,  sizeof(available_units));
  memcpy(&buffer[16], &sectors_per_unit, sizeof(sectors_per_unit));
  memcpy(&buffer[20], &bytes_per_sector, sizeof(bytes_per_sector));

  *output = buffer;
  return 24;
}

static int FillFileSystemFullSizeInfoRaw(uint8_t** output)
{
  uint8_t* buffer;
  uint64_t total_units;
  uint64_t caller_available_units;
  uint64_t actual_available_units;
  uint32_t sectors_per_unit;
  uint32_t bytes_per_sector;

  if ((buffer = (uint8_t*)calloc(1, 32U)) == NULL)
  {
    return -ENOMEM;
  }

  total_units            = FS_TOTAL_UNITS;
  caller_available_units = FS_AVAILABLE_UNITS;
  actual_available_units = FS_AVAILABLE_UNITS;
  sectors_per_unit       = FS_SECTORS_PER_UNIT;
  bytes_per_sector       = FS_BYTES_PER_SECTOR;

  memcpy(&buffer[0],  &total_units,            sizeof(total_units));
  memcpy(&buffer[8],  &caller_available_units, sizeof(caller_available_units));
  memcpy(&buffer[16], &actual_available_units, sizeof(actual_available_units));
  memcpy(&buffer[24], &sectors_per_unit,       sizeof(sectors_per_unit));
  memcpy(&buffer[28], &bytes_per_sector,       sizeof(bytes_per_sector));

  *output = buffer;
  return 32;
}

static int FillFileSystemDeviceInfoRaw(uint8_t** output)
{
  uint8_t* buffer;
  uint32_t device_type;
  uint32_t characteristics;

  if ((buffer = (uint8_t*)calloc(1, 8U)) == NULL)
  {
    return -ENOMEM;
  }

  device_type     = FILE_DEVICE_DISK;
  characteristics = 0U;

  memcpy(&buffer[0], &device_type,     sizeof(device_type));
  memcpy(&buffer[4], &characteristics, sizeof(characteristics));

  *output = buffer;
  return 8;
}

static int FillFileSystemAttributeInfoRaw(uint8_t** output)
{
  static const uint8_t filesystem_name[] = { 'S', 0, 'M', 0, 'B', 0, 'F', 0, 'S', 0 };
  uint8_t* buffer;
  uint32_t filesystem_attributes;
  uint32_t maximum_component_name_length;
  uint32_t filesystem_name_length;

  if ((buffer = (uint8_t*)calloc(1, 12U + sizeof(filesystem_name))) == NULL)
  {
    return -ENOMEM;
  }

  filesystem_attributes         = 0x00000002U;
  maximum_component_name_length = 255U;
  filesystem_name_length        = (uint32_t)sizeof(filesystem_name);

  memcpy(&buffer[0],  &filesystem_attributes,         sizeof(filesystem_attributes));
  memcpy(&buffer[4],  &maximum_component_name_length, sizeof(maximum_component_name_length));
  memcpy(&buffer[8],  &filesystem_name_length,        sizeof(filesystem_name_length));
  memcpy(&buffer[12], filesystem_name,                sizeof(filesystem_name));

  *output = buffer;
  return (int)(12U + sizeof(filesystem_name));
}

static int FillFileSystemInfoRaw(uint8_t info_class, uint8_t** output)
{
  switch (info_class)
  {
    case SMB2_FILE_FS_SIZE_INFORMATION:       return FillFileSystemSizeInfoRaw(output);
    case SMB2_FILE_FS_FULL_SIZE_INFORMATION:  return FillFileSystemFullSizeInfoRaw(output);
    case SMB2_FILE_FS_DEVICE_INFORMATION:     return FillFileSystemDeviceInfoRaw(output);
    case SMB2_FILE_FS_ATTRIBUTE_INFORMATION:  return FillFileSystemAttributeInfoRaw(output);
  }

  return -EINVAL;
}

static int ShouldPassthroughFileSystemInfo(uint8_t info_class)
{
  switch (info_class)
  {
    case SMB2_FILE_FS_SIZE_INFORMATION:
    case SMB2_FILE_FS_FULL_SIZE_INFORMATION:
    case SMB2_FILE_FS_DEVICE_INFORMATION:
    case SMB2_FILE_FS_ATTRIBUTE_INFORMATION:
      return 1;
  }

  return 0;
}

static int FillNodeInfo(enum SombreroNodeType node, const struct SombreroPipeHandle* handle, uint8_t info_type, uint8_t info_class, void** output)
{
  struct smb2_file_basic_info* basic;
  struct smb2_file_standard_info* standard;
  struct smb2_file_all_info* all;
  struct smb2_file_network_open_info* network;
  struct smb2_file_fs_size_info* fs_size;
  struct smb2_file_fs_full_size_info* fs_full_size;
  struct smb2_file_fs_device_info* fs_device;
  struct smb2_file_fs_attribute_info* fs_attribute;
  uint64_t length;

  *output = NULL;
  length  = GetNodeLength(node, handle);

  switch (info_type)
  {
    case SMB2_0_INFO_FILE:
      switch (info_class)
      {
        case SMB2_FILE_BASIC_INFORMATION:
          if ((basic = (struct smb2_file_basic_info*)calloc(1, sizeof(struct smb2_file_basic_info))) == NULL)
            return -ENOMEM;

          FillCurrentTimeval(&basic->creation_time);
          FillCurrentTimeval(&basic->last_access_time);
          FillCurrentTimeval(&basic->last_write_time);
          FillCurrentTimeval(&basic->change_time);
          basic->file_attributes = GetNodeAttributes(node);
          *output = basic;
          return (int)sizeof(struct smb2_file_basic_info);

        case SMB2_FILE_STANDARD_INFORMATION:
          if ((standard = (struct smb2_file_standard_info*)calloc(1, sizeof(struct smb2_file_standard_info))) == NULL)
            return -ENOMEM;

          standard->allocation_size = GetNodeAllocationSize(node, handle);
          standard->end_of_file     = length;
          standard->number_of_links = node == SOMBRERO_NODE_ROOT ? 2U : 1U;
          standard->directory       = (node == SOMBRERO_NODE_ROOT) || (node == SOMBRERO_NODE_PIPE_DIRECTORY);
          *output = standard;
          return (int)sizeof(struct smb2_file_standard_info);

        case SMB2_FILE_ALL_INFORMATION:
          if ((all = (struct smb2_file_all_info*)calloc(1, sizeof(struct smb2_file_all_info))) == NULL)
            return -ENOMEM;

          FillCurrentTimeval(&all->basic.creation_time);
          FillCurrentTimeval(&all->basic.last_access_time);
          FillCurrentTimeval(&all->basic.last_write_time);
          FillCurrentTimeval(&all->basic.change_time);
          all->basic.file_attributes    = GetNodeAttributes(node);
          all->standard.allocation_size = GetNodeAllocationSize(node, handle);
          all->standard.end_of_file     = length;
          all->standard.number_of_links = node == SOMBRERO_NODE_ROOT ? 2U : 1U;
          all->standard.directory       = (node == SOMBRERO_NODE_ROOT) || (node == SOMBRERO_NODE_PIPE_DIRECTORY);
          all->index_number             = GetNodeIndexNumber(node);
          all->ea_size                  = 0U;
          all->access_flags             = node == SOMBRERO_NODE_PIPE
                                           ? (SMB2_GENERIC_READ | SMB2_GENERIC_WRITE)
                                           : SOMBRERO_ROOT_ACCESS;
          all->current_byte_offset      = 0U;
          all->mode                     = 0U;
          all->alignment_requirement    = 0U;
          all->name                     = (const uint8_t*)(node == SOMBRERO_NODE_ROOT ? "\\"
                                                                 : (node == SOMBRERO_NODE_PIPE_DIRECTORY ? "pipe"
                                                                                                         : SOMBRERO_PIPE_NAME));
          *output = all;
          return (int)sizeof(struct smb2_file_all_info);

        case SMB2_FILE_NETWORK_OPEN_INFORMATION:
          if ((network = (struct smb2_file_network_open_info*)calloc(1, sizeof(struct smb2_file_network_open_info))) == NULL)
            return -ENOMEM;

          FillCurrentTimeval(&network->creation_time);
          FillCurrentTimeval(&network->last_access_time);
          FillCurrentTimeval(&network->last_write_time);
          FillCurrentTimeval(&network->change_time);
          network->allocation_size = GetNodeAllocationSize(node, handle);
          network->end_of_file     = length;
          network->file_attributes = GetNodeAttributes(node);
          *output = network;
          return (int)sizeof(struct smb2_file_network_open_info);
      }

      return -EINVAL;

    case SMB2_0_INFO_FILESYSTEM:
      switch (info_class)
      {
        case SMB2_FILE_FS_SIZE_INFORMATION:
          if ((fs_size = (struct smb2_file_fs_size_info*)calloc(1, sizeof(struct smb2_file_fs_size_info))) == NULL)
            return -ENOMEM;

          fs_size->total_allocation_units      = FS_TOTAL_UNITS;
          fs_size->available_allocation_units  = FS_AVAILABLE_UNITS;
          fs_size->sectors_per_allocation_unit = FS_SECTORS_PER_UNIT;
          fs_size->bytes_per_sector            = FS_BYTES_PER_SECTOR;
          *output = fs_size;
          return (int)sizeof(struct smb2_file_fs_size_info);

        case SMB2_FILE_FS_FULL_SIZE_INFORMATION:
          if ((fs_full_size = (struct smb2_file_fs_full_size_info*)calloc(1, sizeof(struct smb2_file_fs_full_size_info))) == NULL)
            return -ENOMEM;

          fs_full_size->total_allocation_units            = FS_TOTAL_UNITS;
          fs_full_size->caller_available_allocation_units = FS_AVAILABLE_UNITS;
          fs_full_size->actual_available_allocation_units = FS_AVAILABLE_UNITS;
          fs_full_size->sectors_per_allocation_unit       = FS_SECTORS_PER_UNIT;
          fs_full_size->bytes_per_sector                  = FS_BYTES_PER_SECTOR;
          *output = fs_full_size;
          return (int)sizeof(struct smb2_file_fs_full_size_info);

        case SMB2_FILE_FS_DEVICE_INFORMATION:
          if ((fs_device = (struct smb2_file_fs_device_info*)calloc(1, sizeof(struct smb2_file_fs_device_info))) == NULL)
            return -ENOMEM;

          fs_device->device_type = FILE_DEVICE_DISK;
          *output = fs_device;
          return (int)sizeof(struct smb2_file_fs_device_info);

        case SMB2_FILE_FS_ATTRIBUTE_INFORMATION:
          if ((fs_attribute = (struct smb2_file_fs_attribute_info*)calloc(1, sizeof(struct smb2_file_fs_attribute_info))) == NULL)
            return -ENOMEM;

          fs_attribute->filesystem_attributes         = 0x00000002U;
          fs_attribute->maximum_component_name_length = 255;
          fs_attribute->filesystem_name               = (const uint8_t*)"Sombrero";
          fs_attribute->filesystem_name_length        = (uint32_t)strlen((const char*)fs_attribute->filesystem_name);
          *output = fs_attribute;
          return (int)sizeof(struct smb2_file_fs_attribute_info);
      }

      return -EINVAL;
  }

  return -EINVAL;
}

static void HandleSombreroError(struct smb2_context* context, const char* error)
{
  struct SombreroSession* session;

  if ((context == NULL) || (error == NULL))
    return;

  fprintf(stderr, "%p: %s\n", (void*)context, error);

  if (((strstr(error, "remote closed connection") != NULL) ||
       (strstr(error, "Read from socket failed") != NULL)) &&
      ((session = GetSombreroSession(context)) != NULL))
    DetachSambarOpaque(&session->sambar, context);
}

static const char* GetCommandName(uint16_t command)
{
  switch (command)
  {
    case SMB1_NEGOTIATE:        return "SMB1_NEGOTIATE";
    case SMB2_NEGOTIATE:        return "SMB2_NEGOTIATE";
    case SMB2_SESSION_SETUP:    return "SMB2_SESSION_SETUP";
    case SMB2_LOGOFF:           return "SMB2_LOGOFF";
    case SMB2_TREE_CONNECT:     return "SMB2_TREE_CONNECT";
    case SMB2_TREE_DISCONNECT:  return "SMB2_TREE_DISCONNECT";
    case SMB2_CREATE:           return "SMB2_CREATE";
    case SMB2_CLOSE:            return "SMB2_CLOSE";
    case SMB2_FLUSH:            return "SMB2_FLUSH";
    case SMB2_READ:             return "SMB2_READ";
    case SMB2_WRITE:            return "SMB2_WRITE";
    case SMB2_LOCK:             return "SMB2_LOCK";
    case SMB2_IOCTL:            return "SMB2_IOCTL";
    case SMB2_CANCEL:           return "SMB2_CANCEL";
    case SMB2_ECHO:             return "SMB2_ECHO";
    case SMB2_QUERY_DIRECTORY:  return "SMB2_QUERY_DIRECTORY";
    case SMB2_CHANGE_NOTIFY:    return "SMB2_CHANGE_NOTIFY";
    case SMB2_QUERY_INFO:       return "SMB2_QUERY_INFO";
    case SMB2_SET_INFO:         return "SMB2_SET_INFO";
  }

  return "UNKNOWN";
}

static void TraceState(const char* stage, struct smb2_context* context)
{
  uint16_t pdu_command;
  uint16_t next_command;

  pdu_command  = context != NULL && context->pdu != NULL      ? context->pdu->header.command      : 0xffffU;
  next_command = context != NULL && context->next_pdu != NULL ? context->next_pdu->header.command : 0xffffU;

  fprintf(stderr,
          "Sombrero[%p]: %s pdu=%s next=%s session=%llu dialect=0x%x in=%d out=%p wait=%p\n",
          (void*)context,
          stage,
          pdu_command  == 0xffffU ? "NULL" : GetCommandName(pdu_command),
          next_command == 0xffffU ? "NULL" : GetCommandName(next_command),
          (unsigned long long)(context != NULL ? context->session_id : 0ULL),
          (unsigned int)(context != NULL ? context->dialect : 0U),
          context != NULL ? context->in.niov : -1,
          context != NULL ? (void*)context->outqueue : NULL,
          context != NULL ? (void*)context->waitqueue : NULL);
}

static uint64_t MakeWindowsTime(const struct smb2_timeval* value)
{
  return ((uint64_t)value->tv_sec + 11644473600ULL) * 10000000ULL + ((uint64_t)value->tv_usec * 10ULL);
}

static void InitializeServer(struct smb2_server* server)
{
  static const char* domain = "WORKGROUP";

  if (!server->max_transact_size)
  {
    server->max_transact_size = 0x100000;
    server->max_read_size     = 0x100000;
    server->max_write_size    = 0x100000;
  }

  if (!server->guid[0])                 memcpy(server->guid, "libsmb2-srvrguid", 16);
  if (!server->hostname[0])             gethostname(server->hostname, sizeof(server->hostname));
  if (!server->domain[0])               strncpy(server->domain, domain, sizeof(server->domain) - 1U);
  if (server->session_counter == 0ULL)  server->session_counter = 0x1234;
}

static void FreeConnectData(struct smb2_context* context, struct connect_data* data)
{
  if (data != NULL)
  {
    if (data->auth_data != NULL)        ntlmssp_destroy_context(data->auth_data);
    if (context->connect_data == data)  context->connect_data = NULL;

    free(data->utf8_unc);
    free(data->utf16_unc);
    free((void*)data->server);
    free((void*)data->share);
    free((void*)data->user);
    free(data);
  }
}

static void QueueReply(struct smb2_context* context, struct smb2_pdu* pdu)
{
  if (pdu != NULL)  smb2_queue_pdu(context, pdu);
}

static struct smb2_pdu* MakeErrorReply(struct smb2_context* context, uint8_t command, int status, void* cb_data)
{
  struct smb2_error_reply reply;

  memset(&reply, 0, sizeof(struct smb2_error_reply));
  return smb2_cmd_error_reply_async(context, &reply, command, status, NULL, cb_data);
}

static int MapErrnoToStatus(uint16_t command, int result)
{
  switch (result)
  {
    case -ENOENT:  return command == SMB2_TREE_CONNECT ? SMB2_STATUS_BAD_NETWORK_NAME : SMB2_STATUS_OBJECT_NAME_NOT_FOUND;
    case -ENOMEM:  return SMB2_STATUS_INSUFFICIENT_RESOURCES;
    case -EINVAL:  return (command == SMB2_IOCTL) || (command == SMB2_SET_INFO) ? SMB2_STATUS_INVALID_DEVICE_REQUEST : SMB2_STATUS_INVALID_PARAMETER;
    case -EPERM:
    case -EACCES:
    case -EROFS:   return SMB2_STATUS_ACCESS_DENIED;
  }

  return SMB2_STATUS_NOT_SUPPORTED;
}

static int PrepareGeneralRequest(struct smb2_context* context, void* cb_data);
static void HandleGeneralRequest(struct smb2_context* context, int status, void* command_data, void* cb_data);
static void HandleSessionSetupRequest(struct smb2_context* context, int status, void* command_data, void* cb_data);
static void HandleNegotiateRequest(struct smb2_context* context, int status, void* command_data, void* cb_data);

static int PrepareGeneralRequest(struct smb2_context* context, void* cb_data)
{
  TraceState("prepare-general:before", context);

  if ((context->next_pdu = smb2_allocate_pdu(context, SMB2_TREE_CONNECT, HandleGeneralRequest, cb_data)) == NULL)
  {
    smb2_set_error(context, "can not alloc pdu for request");
    smb2_close_context(context);
    return -1;
  }

  TraceState("prepare-general:after", context);
  return 0;
}

static void HandleGeneralRequest(struct smb2_context* context, int status, void* command_data, void* cb_data)
{
  struct connect_data* data;
  struct smb2_server* server;
  struct smb2_pdu* pdu;
  int result;

  if (((data   = (struct connect_data*)cb_data) == NULL) ||
      ((server = data->server_context) == NULL))
  {
    fprintf(stderr, "Sombrero[%p]: general request missing server context\n", (void*)context);
    smb2_close_context(context);
    return;
  }

  TraceState("general:enter", context);
  fprintf(stderr, "Sombrero[%p]: general status=%d command=%s\n", (void*)context, status, context->pdu != NULL ? GetCommandName(context->pdu->header.command) : "NULL");

  if (context->pdu == NULL)
  {
    smb2_set_error(context, "No pdu for general client request");
    smb2_close_context(context);
    return;
  }

  if (status == SMB2_STATUS_CANCELLED)
  {
    return;
  }

  pdu    = NULL;
  result = -1;

  switch (context->pdu->header.command)
  {
    case SMB2_LOGOFF:
      result = ((server->handlers != NULL) && (server->handlers->logoff_cmd != NULL)) ? server->handlers->logoff_cmd(server, context) : 0;

      if (result == 0)      pdu = smb2_cmd_logoff_reply_async(context, NULL, cb_data);
      else if (result < 0)  pdu = MakeErrorReply(context, SMB2_LOGOFF, SMB2_STATUS_NOT_IMPLEMENTED, cb_data);

      QueueReply(context, pdu);

      if ((context->next_pdu = smb2_allocate_pdu(context, SMB2_TREE_CONNECT, HandleSessionSetupRequest, cb_data)) == NULL)
      {
        smb2_set_error(context, "can not alloc pdu for authorization session setup request");
        smb2_close_context(context);
      }
      return;

    case SMB2_TREE_CONNECT:
      if ((server->handlers != NULL) &&
          (server->handlers->tree_connect_cmd != NULL))
      {
        struct smb2_tree_connect_reply reply;

        memset(&reply, 0, sizeof(reply));
        result = server->handlers->tree_connect_cmd(server, context, (struct smb2_tree_connect_request*)command_data, &reply);

        if (result == 0)      pdu = smb2_cmd_tree_connect_reply_async(context, &reply, 0, NULL, cb_data);
        else if (result < 0)  pdu = MakeErrorReply(context, SMB2_TREE_CONNECT, MapErrnoToStatus(SMB2_TREE_CONNECT, result), cb_data);
      }
      else
      {
        pdu = MakeErrorReply(context, SMB2_TREE_CONNECT, SMB2_STATUS_NOT_SUPPORTED, cb_data);
      }
      break;

    case SMB2_TREE_DISCONNECT:
      result = ((server->handlers != NULL) && (server->handlers->tree_disconnect_cmd != NULL)) ? server->handlers->tree_disconnect_cmd(server, context, context->hdr.sync.tree_id) : 0;

      if (result == 0)      pdu = smb2_cmd_tree_disconnect_reply_async(context, NULL, cb_data);
      else if (result < 0)  pdu = MakeErrorReply(context, SMB2_TREE_DISCONNECT, SMB2_STATUS_NOT_IMPLEMENTED, cb_data);

      smb2_disconnect_tree_id(context, context->hdr.sync.tree_id);
      break;

    case SMB2_CREATE:
      if ((server->handlers != NULL) &&
          (server->handlers->create_cmd != NULL))
      {
        struct smb2_create_request* request;
        struct smb2_create_reply reply;

        request = (struct smb2_create_request*)command_data;
        memset(&reply, 0, sizeof(reply));
        result = server->handlers->create_cmd(server, context, request, &reply);

        if (result == 0)      pdu = smb2_cmd_create_reply_async(context, &reply, NULL, cb_data);
        else if (result < 0)  pdu = MakeErrorReply(context, SMB2_CREATE, MapErrnoToStatus(SMB2_CREATE, result), cb_data);

        if (request->name != NULL)  smb2_free_data(context, (void*)request->name);
      }
      else
      {
        pdu = MakeErrorReply(context, SMB2_CREATE, SMB2_STATUS_NOT_SUPPORTED, cb_data);
      }
      break;

    case SMB2_CLOSE:
      if ((server->handlers != NULL) &&
          (server->handlers->close_cmd != NULL))
      {
        struct smb2_close_reply reply;

        memset(&reply, 0, sizeof(reply));
        result = server->handlers->close_cmd(server, context, (struct smb2_close_request*)command_data, &reply);

        if (result == 0)      pdu = smb2_cmd_close_reply_async(context, &reply, NULL, cb_data);
        else if (result < 0)  pdu = MakeErrorReply(context, SMB2_CLOSE, MapErrnoToStatus(SMB2_CLOSE, result), cb_data);
      }
      else
      {
        pdu = MakeErrorReply(context, SMB2_CLOSE, SMB2_STATUS_NOT_SUPPORTED, cb_data);
      }
      break;

    case SMB2_FLUSH:
      result = ((server->handlers != NULL) && (server->handlers->flush_cmd != NULL)) ? server->handlers->flush_cmd(server, context, (struct smb2_flush_request*)command_data) : 0;

      if (result == 0)      pdu = smb2_cmd_flush_reply_async(context, NULL, cb_data);
      else if (result < 0)  pdu = MakeErrorReply(context, SMB2_FLUSH, MapErrnoToStatus(SMB2_FLUSH, result), cb_data);
      break;

    case SMB2_READ:
      if ((server->handlers != NULL) &&
          (server->handlers->read_cmd != NULL))
      {
        struct smb2_read_reply reply;

        memset(&reply, 0, sizeof(reply));
        result = server->handlers->read_cmd(server, context, (struct smb2_read_request*)command_data, &reply);

        if (result == 0)      pdu = smb2_cmd_read_reply_async(context, &reply, NULL, cb_data);
        else if (result < 0)  pdu = MakeErrorReply(context, SMB2_READ, MapErrnoToStatus(SMB2_READ, result), cb_data);
      }
      else
      {
        pdu = MakeErrorReply(context, SMB2_READ, SMB2_STATUS_NOT_SUPPORTED, cb_data);
      }
      break;

    case SMB2_WRITE:
      if ((server->handlers != NULL) &&
          (server->handlers->write_cmd != NULL))
      {
        struct smb2_write_reply reply;

        memset(&reply, 0, sizeof(reply));
        result = server->handlers->write_cmd(server, context, (struct smb2_write_request*)command_data, &reply);

        if (result == 0)      pdu = smb2_cmd_write_reply_async(context, &reply, NULL, cb_data);
        else if (result < 0)  pdu = MakeErrorReply(context, SMB2_WRITE, MapErrnoToStatus(SMB2_WRITE, result), cb_data);
      }
      else
      {
        pdu = MakeErrorReply(context, SMB2_WRITE, SMB2_STATUS_NOT_SUPPORTED, cb_data);
      }
      break;

    case SMB2_LOCK:
      result = ((server->handlers != NULL) && (server->handlers->lock_cmd != NULL)) ? server->handlers->lock_cmd(server, context, (struct smb2_lock_request*)command_data) : 0;

      if (result == 0)      pdu = smb2_cmd_lock_reply_async(context, NULL, cb_data);
      else if (result < 0)  pdu = MakeErrorReply(context, SMB2_LOCK, MapErrnoToStatus(SMB2_LOCK, result), cb_data);
      break;

    case SMB2_IOCTL:
      if ((server->handlers != NULL) &&
          (server->handlers->ioctl_cmd != NULL))
      {
        struct smb2_ioctl_reply reply;

        memset(&reply, 0, sizeof(reply));
        result = server->handlers->ioctl_cmd(server, context, (struct smb2_ioctl_request*)command_data, &reply);

        if (result == 0)      pdu = smb2_cmd_ioctl_reply_async(context, &reply, NULL, cb_data);
        else if (result < 0)  pdu = MakeErrorReply(context, SMB2_IOCTL, MapErrnoToStatus(SMB2_IOCTL, result), cb_data);
      }
      else
      {
        pdu = MakeErrorReply(context, SMB2_IOCTL, SMB2_STATUS_NOT_SUPPORTED, cb_data);
      }
      break;

    case SMB2_CANCEL:
      result = ((server->handlers != NULL) && (server->handlers->cancel_cmd != NULL)) ? server->handlers->cancel_cmd(server, context) : 0;
      if (result < 0)  pdu = MakeErrorReply(context, SMB2_CANCEL, MapErrnoToStatus(SMB2_CANCEL, result), cb_data);
      break;

    case SMB2_ECHO:
      result =  ((server->handlers != NULL) && (server->handlers->echo_cmd != NULL)) ? server->handlers->echo_cmd(server, context) : 0;

      if (result == 0)      pdu = smb2_cmd_echo_reply_async(context, NULL, cb_data);
      else if (result < 0)  pdu = MakeErrorReply(context, SMB2_ECHO, MapErrnoToStatus(SMB2_ECHO, result), cb_data);
      break;

    case SMB2_QUERY_DIRECTORY:
      if ((server->handlers != NULL) &&
          (server->handlers->query_directory_cmd != NULL))
      {
        struct smb2_query_directory_request* request;
        struct smb2_query_directory_reply reply;

        request = (struct smb2_query_directory_request*)command_data;
        memset(&reply, 0, sizeof(reply));
        result = server->handlers->query_directory_cmd(server, context, request, &reply);

        if (result < 0)        pdu = MakeErrorReply(context, SMB2_QUERY_DIRECTORY, MapErrnoToStatus(SMB2_QUERY_DIRECTORY, result), cb_data);
        else if (result == 0)  pdu = reply.output_buffer_length == 0 ? MakeErrorReply(context, SMB2_QUERY_DIRECTORY, SMB2_STATUS_NO_MORE_FILES, cb_data)
                                                                     : smb2_cmd_query_directory_reply_async(context, request, &reply, NULL, cb_data);

        if (request->name != NULL)  smb2_free_data(context, (void*)request->name);
      }
      else
      {
        pdu = MakeErrorReply(context, SMB2_QUERY_DIRECTORY, SMB2_STATUS_NOT_SUPPORTED, cb_data);
      }
      break;

    case SMB2_CHANGE_NOTIFY:
      if ((server->handlers != NULL) &&
          (server->handlers->change_notify_cmd != NULL))
      {
        struct smb2_change_notify_reply reply;

        memset(&reply, 0, sizeof(reply));
        result = server->handlers->change_notify_cmd(server, context, (struct smb2_change_notify_request*)command_data, &reply);

        if (result < 0)        pdu = MakeErrorReply(context, SMB2_CHANGE_NOTIFY, MapErrnoToStatus(SMB2_CHANGE_NOTIFY, result), cb_data);
        else if (result == 0)  pdu = smb2_cmd_change_notify_reply_async(context, &reply, NULL, cb_data);
      }
      break;

    case SMB2_QUERY_INFO:
      if ((server->handlers != NULL) &&
          (server->handlers->query_info_cmd != NULL))
      {
        struct smb2_query_info_request* request;
        struct smb2_query_info_request request_for_reply;
        struct smb2_query_info_request* reply_request;
        struct smb2_query_info_reply reply;
        int passthrough;

        request = (struct smb2_query_info_request*)command_data;
        request_for_reply = *request;
        reply_request = request;
        memset(&reply, 0, sizeof(reply));
        result = server->handlers->query_info_cmd(server, context, request, &reply);

        if (result < 0)        pdu = MakeErrorReply(context, SMB2_QUERY_INFO, MapErrnoToStatus(SMB2_QUERY_INFO, result), cb_data);
        else if (result == 0)
        {
          if (reply.output_buffer_length <= 0)
          {
            pdu = MakeErrorReply(context, SMB2_QUERY_INFO, SMB2_STATUS_INVALID_INFO_CLASS, cb_data);
          }
          else
          {
            passthrough = context->passthrough;

            if ((request->info_type == SMB2_0_INFO_FILESYSTEM) &&
                ShouldPassthroughFileSystemInfo(request->file_info_class))
            {
              context->passthrough = 1;
              request_for_reply.file_info_class = 0xffU;
              reply_request = &request_for_reply;
            }

            pdu = smb2_cmd_query_info_reply_async(context, reply_request, &reply, NULL, cb_data);
            context->passthrough = passthrough;
          }
        }
      }
      else
      {
        pdu = MakeErrorReply(context, SMB2_QUERY_INFO, SMB2_STATUS_NOT_SUPPORTED, cb_data);
      }
      break;

    case SMB2_SET_INFO:
      result = ((server->handlers != NULL) && (server->handlers->set_info_cmd != NULL)) ? server->handlers->set_info_cmd(server, context, (struct smb2_set_info_request*)command_data) : 0;

      if (result < 0)        pdu = MakeErrorReply(context, SMB2_SET_INFO, MapErrnoToStatus(SMB2_SET_INFO, result), cb_data);
      else if (result == 0)  pdu = smb2_cmd_set_info_reply_async(context, (struct smb2_set_info_request*)command_data, NULL, cb_data);
      break;

    default:
      smb2_set_error(context, "Client request %d not implemented %s", context->pdu->header.command, smb2_get_error(context));
      break;
  }

  QueueReply(context, pdu);
  PrepareGeneralRequest(context, cb_data);
}

static void HandleSessionSetupRequest(struct smb2_context* context, int status, void* command_data, void* cb_data)
{
  struct connect_data* data;
  struct smb2_server* server;
  struct smb2_session_setup_request* request;
  struct smb2_session_setup_reply reply;
  struct smb2_pdu* pdu;
  uint32_t message_type;
  uint8_t* response_token;
  int response_length;
  int wrapped;
  int have_valid_session_key;
  int result;
  int more;

  if ((status != 0) ||
      ((data = (struct connect_data*)cb_data) == NULL) ||
      ((server = data->server_context) == NULL))
  {
    fprintf(stderr, "Sombrero[%p]: session setup aborted status=%d server=%p\n", (void*)context, status, (void*)server);
    return;
  }

  TraceState("session-setup:enter", context);
  request = (struct smb2_session_setup_request*)command_data;
  pdu     = NULL;
  more    = 0;
  have_valid_session_key = 1;
  memset(&reply, 0, sizeof(reply));

  smb3_update_preauth_hash(context, context->in.niov - 1, &context->in.iov[1]);

  if (context->sec != SMB2_SEC_NTLMSSP)
  {
    smb2_set_error(context, "Unsupported security mode");
    smb2_close_context(context);
    return;
  }

  if (ntlmssp_get_message_type(context, request->security_buffer, request->security_buffer_length, &message_type, &response_token, &response_length, &wrapped) < 0)
  {
    smb2_set_error(context, "No message type in NTLMSSP %s", smb2_get_error(context));
    smb2_close_context(context);
    return;
  }

  fprintf(stderr, "Sombrero[%p]: session setup message_type=%u wrapped=%d response_length=%d\n", (void*)context, message_type, wrapped, response_length);

  if (message_type == NEGOTIATE_MESSAGE)
  {
    if (data->auth_data != NULL)
      ntlmssp_destroy_context(data->auth_data);

    data->auth_data = ntlmssp_init_context("", "", "", server->hostname, context->client_challenge);

    if (data->auth_data == NULL)
    {
      smb2_set_error(context, "can not init auth data %s", smb2_get_error(context));
      smb2_close_context(context);
      return;
    }

    context->connect_data = data;
    context->session_id   = server->session_counter++;
    more                  = 1;

    if ((context->next_pdu = smb2_allocate_pdu(context, SMB2_SESSION_SETUP, HandleSessionSetupRequest, cb_data)) == NULL)
    {
      smb2_set_error(context, "can not alloc pdu for authorization session setup request");
      smb2_close_context(context);
      return;
    }
  }
  else if (message_type == AUTHENTICATION_MESSAGE)
  {
    if ((context->next_pdu = smb2_allocate_pdu(context, SMB2_TREE_CONNECT, HandleGeneralRequest, cb_data)) == NULL)
    {
      smb2_set_error(context, "can not alloc pdu for request");
      smb2_close_context(context);
      return;
    }
  }
  else
  {
    smb2_set_error(context, "Unexpected ntlmssp msg code %08X", message_type);
    smb2_close_context(context);
    return;
  }

  if (ntlmssp_generate_blob(server,
                            context,
                            0,
                            data->auth_data,
                            request->security_buffer,
                            request->security_buffer_length,
                            &reply.security_buffer,
                            &reply.security_buffer_length) < 0)
  {
    fprintf(stderr,
            "Sombrero[%p]: ntlmssp_generate_blob failed message_type=%u wrapped=%d security_buffer_length=%u response_length=%d\n",
            (void*)context,
            message_type,
            wrapped,
            request->security_buffer_length,
            response_length);
    smb2_close_context(context);
    return;
  }

  fprintf(stderr,
          "Sombrero[%p]: ntlmssp_generate_blob ok message_type=%u reply_length=%u\n",
          (void*)context,
          message_type,
          reply.security_buffer_length);

  if ((message_type == AUTHENTICATION_MESSAGE) &&
      !ntlmssp_get_authenticated(data->auth_data))
  {
    pdu = MakeErrorReply(context, SMB2_SESSION_SETUP, SMB2_STATUS_LOGON_FAILURE, cb_data);

    if (context->next_pdu != NULL)
    {
      smb2_free_pdu(context, context->next_pdu);
      context->next_pdu = smb2_allocate_pdu(context, SMB2_SESSION_SETUP, HandleSessionSetupRequest, cb_data);
    }
  }
  else
  {
    if ((message_type == AUTHENTICATION_MESSAGE) && (ntlmssp_get_session_key(data->auth_data, &context->session_key, &context->session_key_size) < 0))  have_valid_session_key = 0;
    if (server->signing_enabled && have_valid_session_key && (context->dialect >= SMB2_VERSION_0311))                                                   context->sign = 1;
    if (server->allow_anonymous && ((context->user == NULL || context->user[0] == '\0') || (strcmp(context->user, "GUEST") == 0)))                      reply.session_flags |= SMB2_SESSION_FLAG_IS_GUEST;

    if (context->sign && !have_valid_session_key)
    {
      smb2_set_error(context, "Signing required by server. Session key is not available %s", smb2_get_error(context));
      smb2_close_context(context);
      free(reply.security_buffer);
      return;
    }

    if (context->sign)
    {
      CreateSigningKey(context);
    }

    fprintf(stderr,
            "Sombrero[%p]: session reply flags=0x%x sign=%u sec_mode=0x%x dialect=0x%x reply_len=%u user=%s\n",
            (void*)context,
            reply.session_flags,
            context->sign,
            context->security_mode,
            context->dialect,
            reply.security_buffer_length,
            context->user != NULL ? context->user : "(null)");

    pdu = smb2_cmd_session_setup_reply_async(context, &reply, NULL, cb_data);

    if (pdu != NULL)
    {
      if (more)
      {
        pdu->header.status = SMB2_STATUS_MORE_PROCESSING_REQUIRED;
      }
      else if ((server->handlers != NULL) &&
               (server->handlers->session_established != NULL) &&
               ((result = server->handlers->session_established(server, context)) != 0))
      {
        fprintf(stderr, "Sombrero[%p]: session_established failed result=%d\n", (void*)context, result);
        smb2_set_error(context, "server session start handler failed");
        smb2_close_context(context);
        return;
      }
      else
      {
        fprintf(stderr, "Sombrero[%p]: session_established ok\n", (void*)context);
      }
    }
  }

  QueueReply(context, pdu);
  if (pdu != NULL)  smb3_update_preauth_hash(context, pdu->out.niov, &pdu->out.iov[0]);

  TraceState("session-setup:queued", context);
}

static void HandleNegotiateRequest(struct smb2_context* context, int status, void* command_data, void* cb_data)
{
  struct connect_data* data;
  struct smb2_server* server;
  struct smb2_negotiate_request* request;
  struct smb2_negotiate_reply reply;
  struct smb2_pdu* pdu;
  struct smb2_timeval now;
  uint16_t dialects[5];
  int dialect_count;
  int dialect_index;
  int index;

  if ((status != 0) ||
      ((data = (struct connect_data*)cb_data) == NULL) ||
      ((server = data->server_context) == NULL))
  {
    fprintf(stderr, "Sombrero[%p]: negotiate aborted status=%d server=%p\n", (void*)context, status, (void*)server);
    return;
  }

  TraceState("negotiate:enter", context);
  request = (struct smb2_negotiate_request*)command_data;
  pdu     = NULL;
  memset(&reply, 0, sizeof(reply));
  smb2_set_error(context, "");

  smb3_init_preauth_hash(context);
  smb3_update_preauth_hash(context, context->in.niov - 1, &context->in.iov[1]);

  switch (context->version)
  {
    case SMB2_VERSION_ANY:
      dialects[0]   = SMB2_VERSION_0202;
      dialects[1]   = SMB2_VERSION_0210;
      dialects[2]   = SMB2_VERSION_0300;
      dialects[3]   = SMB2_VERSION_0302;
      dialects[4]   = SMB2_VERSION_0311;
      dialect_count = 5;
      break;

    case SMB2_VERSION_ANY2:
      dialects[0]   = SMB2_VERSION_0202;
      dialects[1]   = SMB2_VERSION_0210;
      dialect_count = 2;
      break;

    case SMB2_VERSION_ANY3:
      dialects[0]   = SMB2_VERSION_0300;
      dialects[1]   = SMB2_VERSION_0302;
      dialects[2]   = SMB2_VERSION_0311;
      dialect_count = 3;
      break;

    default:
      dialects[0]   = context->version;
      dialect_count = 1;
      break;
  }

  if ((request != NULL) &&
      (context->pdu->header.command != SMB1_NEGOTIATE))
  {
    if (request->dialect_count == 0)
    {
      context->next_pdu = smb2_allocate_pdu(context, SMB2_NEGOTIATE, HandleNegotiateRequest, cb_data);
      pdu               = MakeErrorReply(context, SMB2_NEGOTIATE, SMB2_STATUS_INVALID_PARAMETER, cb_data);
      QueueReply(context, pdu);
      return;
    }

    context->dialect = 0;

    for (dialect_index = request->dialect_count - 1; dialect_index >= 0; dialect_index--)
    {
      for (index = dialect_count - 1; index >= 0; index--)
      {
        if (dialects[index] == request->dialects[dialect_index])
        {
          context->dialect = dialects[index];
          break;
        }
      }

      if (context->dialect != 0)
      {
        break;
      }
    }

    if (dialect_index < 0)
    {
      smb2_set_error(context, "No common dialects for protocol");
      smb2_close_context(context);
      return;
    }

    smb2_set_client_guid(context, request->client_guid);
    fprintf(stderr, "Sombrero[%p]: negotiate dialect_count=%d selected=0x%x capabilities=0x%x\n", (void*)context, request->dialect_count, context->dialect, request->capabilities);
    reply.capabilities = SMB2_GLOBAL_CAP_LARGE_MTU;

    if ((context->version == SMB2_VERSION_ANY)  ||
        (context->version == SMB2_VERSION_ANY3) ||
        (context->version == SMB2_VERSION_0300) ||
        (context->version == SMB2_VERSION_0302) ||
        (context->version == SMB2_VERSION_0311))
    {
      reply.capabilities |= SMB2_GLOBAL_CAP_ENCRYPTION;
    }

    if ((context->dialect > SMB2_VERSION_0202) &&
        (request->capabilities & SMB2_GLOBAL_CAP_LARGE_MTU))
    {
      context->supports_multi_credit = 1;
    }
  }
  else
  {
    context->dialect = SMB2_VERSION_WILDCARD;
  }

  reply.security_mode     = server->signing_enabled ? (SMB2_NEGOTIATE_SIGNING_ENABLED | SMB2_NEGOTIATE_SIGNING_REQUIRED) : 0;
  reply.max_transact_size = context->max_transact_size;
  reply.max_read_size     = context->max_read_size;
  reply.max_write_size    = context->max_write_size;
  reply.dialect_revision  = context->dialect;
  reply.cypher            = context->cypher;

  memcpy(reply.server_guid, server->guid, 16);

  context->capabilities  = reply.capabilities;
  context->security_mode = reply.security_mode;
  context->sec           = SMB2_SEC_NTLMSSP;

  now.tv_sec             = time(NULL);
  now.tv_usec            = 0;
  reply.system_time      = MakeWindowsTime(&now);
  now.tv_sec             = 0;
  reply.server_start_time = MakeWindowsTime(&now);

  reply.security_buffer_length = smb2_spnego_create_negotiate_reply_blob(context, (void*)&reply.security_buffer);
  pdu                          = smb2_cmd_negotiate_reply_async(context, &reply, NULL, cb_data);
  fprintf(stderr, "Sombrero[%p]: negotiate reply security_buffer_length=%d pdu=%p\n", (void*)context, reply.security_buffer_length, (void*)pdu);

  free(reply.security_buffer);
  QueueReply(context, pdu);

  if (pdu != NULL)  smb3_update_preauth_hash(context, pdu->out.niov, &pdu->out.iov[0]);

  if (request != NULL)  context->next_pdu = smb2_allocate_pdu(context, SMB2_SESSION_SETUP, HandleSessionSetupRequest, cb_data);
  else                  context->next_pdu = smb2_allocate_pdu(context, SMB2_NEGOTIATE,     HandleNegotiateRequest,    cb_data);

  if (context->next_pdu == NULL)
  {
    smb2_set_error(context, "can not alloc pdu for next request");
    smb2_close_context(context);
  }

  TraceState("negotiate:queued", context);
}

static int HandleDestroyPipeServer(struct smb2_server* server, struct smb2_context* context)
{
  struct SombreroSession* session;

  if ((server == NULL) ||
      ((session = GetSombreroSession(context)) != NULL))
  {
    if (session != NULL)
    {
      DetachSambarOpaque(&session->sambar, context);
      FreeSombreroHandles(session);
      free(session);
    }
  }

  return 0;
}

static int HandleAuthorizePipeServer(struct smb2_server* server, struct smb2_context* context, const char* user, const char* domain, const char* workstation)
{
  if ((server == NULL) ||
      (context == NULL))
    return -EINVAL;

  if ((user == NULL) ||
      (user[0] == '\0') ||
      (strcasecmp(user, "guest") == 0))
  {
    smb2_set_user(context, "GUEST");
    smb2_set_password(context, "");
  }
  else
  {
    smb2_set_user(context, user);
  }

  if (((domain != NULL) && (domain[0] != '\0')) ||
      ((workstation != NULL) && (workstation[0] != '\0')))
  {
    fprintf(stderr,
            "Sombrero[%p]: authorize user=%s domain=%s workstation=%s\n",
            (void*)context,
            context->user != NULL ? context->user : "(null)",
            domain != NULL ? domain : "",
            workstation != NULL ? workstation : "");
  }

  return 0;
}

static int HandlePipeSessionEstablished(struct smb2_server* server, struct smb2_context* context)
{
  struct SombreroSession* session;

  if ((server == NULL) || ((session = GetSombreroSession(context)) == NULL))
    return -EINVAL;

  session->tree_connected = 0;
  return 0;
}

static int HandlePipeTreeConnect(struct smb2_server* server, struct smb2_context* context, struct smb2_tree_connect_request* request, struct smb2_tree_connect_reply* reply)
{
  struct SombreroSession* session;

  if ((server == NULL) || ((session = GetSombreroSession(context)) == NULL))
    return -EINVAL;

  if ((request == NULL) || !MatchTerminalComponentUTF16(request->path, request->path_length / 2U, SOMBRERO_SHARE_NAME))
  {
    fprintf(stderr, "Sombrero[%p]: tree_connect rejected path_length=%u\n", (void*)context, request != NULL ? request->path_length : 0U);
    return -ENOENT;
  }

  memset(reply, 0, sizeof(struct smb2_tree_connect_reply));
  reply->share_type       = SMB2_SHARE_TYPE_PIPE;
  reply->share_flags      = SMB2_SHAREFLAG_NO_CACHING;
  reply->maximal_access   = SOMBRERO_ROOT_ACCESS;
  session->tree_connected = 1;
  fprintf(stderr, "Sombrero[%p]: tree_connect accepted IPC$\n", (void*)context);
  return 0;
}

static int HandlePipeTreeDisconnect(struct smb2_server* server, struct smb2_context* context, uint32_t tree_id)
{
  struct SombreroSession* session;

  if ((server == NULL) || ((session = GetSombreroSession(context)) == NULL))
    return -EINVAL;

  FreeSombreroHandles(session);
  session->tree_connected = 0;
  fprintf(stderr, "Sombrero[%p]: tree_disconnect tree_id=%u\n", (void*)context, tree_id);
  return 0;
}

static int HandlePipeCreate(struct smb2_server* server, struct smb2_context* context, struct smb2_create_request* request, struct smb2_create_reply* reply)
{
  struct SombreroSession* session;
  struct SombreroPipeHandle* handle;
  enum SombreroNodeType node;

  if ((server == NULL) ||
      ((session = GetSombreroSession(context)) == NULL) ||
      (request == NULL) ||
      (!session->tree_connected))
  {
    fprintf(stderr,
            "Sombrero[%p]: create rejected tree=%u name=%s\n",
            (void*)context,
            session != NULL ? session->tree_connected : 0U,
            (request != NULL) && (request->name != NULL) ? request->name : "(null)");
    return -ENOENT;
  }

  if (MatchIpcRootPath(request->name))             node = SOMBRERO_NODE_ROOT;
  else if (MatchPipeDirectoryPath(request->name))  node = SOMBRERO_NODE_PIPE_DIRECTORY;
  else if (MatchNamedPipePath(request->name))      node = SOMBRERO_NODE_PIPE;
  else
  {
    fprintf(stderr,
            "Sombrero[%p]: create rejected tree=%u name=%s\n",
            (void*)context,
            session->tree_connected,
            request->name != NULL ? request->name : "(null)");
    return -ENOENT;
  }

  if ((handle = (struct SombreroPipeHandle*)calloc(1, sizeof(struct SombreroPipeHandle))) == NULL)
  {
    return -ENOMEM;
  }

  handle->kind = (uint8_t)node;
  FillSombreroPipeFileId(session, handle->file_id);
  handle->next      = session->handles;
  session->handles  = handle;
  if (node == SOMBRERO_NODE_ROOT)                 FillRootCreateReply(reply, handle->file_id);
  else if (node == SOMBRERO_NODE_PIPE_DIRECTORY)  FillPipeDirectoryCreateReply(reply, handle->file_id);
  else                                            FillPipeCreateReply(reply, handle->file_id);
  fprintf(stderr, "Sombrero[%p]: create accepted kind=%s name=%s handle=%llu\n",
          (void*)context,
          node == SOMBRERO_NODE_ROOT ? "root" : (node == SOMBRERO_NODE_PIPE_DIRECTORY ? "pipe-dir" : "pipe"),
          request->name != NULL ? request->name : "(null)",
          (unsigned long long)session->next_handle_id - 1ULL);
  return 0;
}

static int HandlePipeClose(struct smb2_server* server, struct smb2_context* context, struct smb2_close_request* request, struct smb2_close_reply* reply)
{
  struct SombreroSession* session;
  struct SombreroPipeHandle* handle;

  if ((server == NULL) ||
      ((session = GetSombreroSession(context)) == NULL) ||
      (request == NULL) ||
      ((handle = FindSombreroHandle(session, request->file_id)) == NULL))
    return -ENOENT;

  FillPipeCloseReply(handle, reply);
  RemoveSombreroHandle(session, handle);
  return 0;
}

static int HandlePipeFlush(struct smb2_server* server, struct smb2_context* context, struct smb2_flush_request* request)
{
  struct SombreroSession* session;

  if ((server == NULL) ||
      (request == NULL))
    return -EINVAL;

  session = GetSombreroSession(context);
  return session != NULL ? 0 : -EINVAL;
}

static int HandlePipeRead(struct smb2_server* server, struct smb2_context* context, struct smb2_read_request* request, struct smb2_read_reply* reply)
{
  struct SombreroSession* session;
  struct SombreroPipeHandle* handle;
  uint32_t available;
  uint64_t offset;
  uint8_t* data;
  enum SombreroNodeType node;

  if ((server == NULL) ||
      ((session = GetSombreroSession(context)) == NULL) ||
      (request == NULL) ||
      ((handle = FindSombreroHandle(session, request->file_id)) == NULL))
    return -ENOENT;

  node = GetNodeByHandle(handle);

  if (node != SOMBRERO_NODE_PIPE)
    return -EINVAL;

  memset(reply, 0, sizeof(struct smb2_read_reply));

  offset = 0U;
  fprintf(stderr,
          "Sombrero[%p]: read pipe len=%u req_len=%u req_off=%llu\n",
          (void*)context,
          (unsigned)handle->length,
          (unsigned)request->length,
          (unsigned long long)request->offset);

  if ((uint64_t)handle->length <= offset)
    return 0;

  available = handle->length - (uint32_t)offset;

  if (available > request->length)                   available = request->length;
  if ((data = (uint8_t*)malloc(available)) == NULL)  return -ENOMEM;

  memcpy(data, handle->buffer + offset, available);
  reply->data_length = available;
  reply->data        = data;

  if (available > 0U)
  {
    if (available < handle->length)  memmove(handle->buffer, handle->buffer + available, handle->length - available);
    handle->length -= available;
  }

  return 0;
}

static int HandlePipeWrite(struct smb2_server* server, struct smb2_context* context, struct smb2_write_request* request, struct smb2_write_reply* reply)
{
  struct SombreroSession* session;
  struct SombreroPipeHandle* handle;
  int result;
  enum SombreroNodeType node;

  if ((server == NULL) ||
      ((session = GetSombreroSession(context)) == NULL) ||
      (request == NULL) ||
      ((handle = FindSombreroHandle(session, request->file_id)) == NULL))
    return -ENOENT;

  node = GetNodeByHandle(handle);

  if (node != SOMBRERO_NODE_PIPE)
    return -EINVAL;

  fprintf(stderr,
          "Sombrero[%p]: write pipe req_len=%u req_off=%llu\n",
          (void*)context,
          (unsigned)request->length,
          (unsigned long long)request->offset);

  if ((result = SetSombreroHandlePayload(handle, request->buf, request->length)) < 0)
    return result;

  memset(reply, 0, sizeof(struct smb2_write_reply));
  reply->count = request->length;
  return 0;
}

static int HandlePipeLock(struct smb2_server* server, struct smb2_context* context, struct smb2_lock_request* request)
{
  if ((server == NULL) || (request == NULL))
    return -EINVAL;

  return GetSombreroSession(context) != NULL ? 0 : -EINVAL;
}

static int HandlePipeIoctl(struct smb2_server* server, struct smb2_context* context, struct smb2_ioctl_request* request, struct smb2_ioctl_reply* reply)
{
  struct SombreroSession* session;
  struct SombreroPipeHandle* handle;
  uint32_t output_length;
  uint8_t* output;
  int result;
  enum SombreroNodeType node;

  if ((server == NULL) || ((session = GetSombreroSession(context)) == NULL) || (request == NULL))
    return -ENOENT;

  memset(reply, 0, sizeof(struct smb2_ioctl_reply));
  reply->ctl_code = request->ctl_code;
  memcpy(reply->file_id, request->file_id, SMB2_FD_SIZE);

  fprintf(stderr, "Sombrero[%p]: ioctl ctl=0x%x max_out=%u input=%u\n",
          (void*)context,
          (unsigned)request->ctl_code,
          (unsigned)request->max_output_response,
          (unsigned)request->input_count);

  if (request->ctl_code == SMB2_FSCTL_VALIDATE_NEGOTIATE_INFO)           return -ENOENT;
  if (request->ctl_code == 0x001401fcU)                                  return -ENOENT;
  if ((handle = FindSombreroHandle(session, request->file_id)) == NULL)  return -ENOENT;

  node = GetNodeByHandle(handle);

  if (node != SOMBRERO_NODE_PIPE)                                                             return -EINVAL;
  if (request->ctl_code != SMB2_FSCTL_PIPE_TRANSCEIVE)                                        return -EINVAL;
  if ((result = SetSombreroHandlePayload(handle, request->input, request->input_count)) < 0)  return result;

  output_length = request->input_count;
  if (output_length > request->max_output_response)
    output_length = request->max_output_response;

  output = NULL;

  if ((output_length > 0U) && ((output = (uint8_t*)malloc(output_length)) == NULL))
    return -ENOMEM;

  if (output_length > 0U)
    memcpy(output, request->input, output_length);

  reply->output       = output;
  reply->output_count = output_length;
  return 0;
}

static int HandlePipeCancel(struct smb2_server* server, struct smb2_context* context)
{
  if (server == NULL)  return -EINVAL;
  return GetSombreroSession(context) != NULL ? 0 : -EINVAL;
}

static int HandlePipeEcho(struct smb2_server* server, struct smb2_context* context)
{
  if (server == NULL)  return -EINVAL;
  return GetSombreroSession(context) != NULL ? 0 : -EINVAL;
}

static int HandlePipeQueryInfo(struct smb2_server* server, struct smb2_context* context, struct smb2_query_info_request* request, struct smb2_query_info_reply* reply)
{
  struct SombreroSession* session;
  struct SombreroPipeHandle* handle;
  enum SombreroNodeType node;

  if ((server == NULL) ||
      ((session = GetSombreroSession(context)) == NULL) ||
      (request == NULL) ||
      (reply == NULL))
    return -EINVAL;

  handle = request->info_type == SMB2_0_INFO_FILESYSTEM ? NULL : FindSombreroHandle(session, request->file_id);

  if (request->info_type == SMB2_0_INFO_FILESYSTEM)                       node = SOMBRERO_NODE_ROOT;
  else if ((handle == NULL) && (request->info_type == SMB2_0_INFO_FILE))  node = SOMBRERO_NODE_ROOT;
  else if ((handle == NULL) && IsZeroFileId(request->file_id))            node = SOMBRERO_NODE_ROOT;
  else                                                                    node = GetNodeByHandle(handle);

  fprintf(stderr,
          "Sombrero[%p]: query_info type=%u class=%u node=%d out=%u\n",
          (void*)context,
          (unsigned)request->info_type,
          (unsigned)request->file_info_class,
          (int)node,
          (unsigned)request->output_buffer_length);

  if (node == SOMBRERO_NODE_NONE)
    return -ENOENT;

  if ((node == SOMBRERO_NODE_ROOT) &&
      (request->info_type == SMB2_0_INFO_FILE) &&
      (request->file_info_class == SMB2_FILE_ALL_INFORMATION))
  {
    fprintf(stderr,
            "Sombrero[%p]: query_info forcing ENOENT for IPC$ root FILE_ALL_INFORMATION\n",
            (void*)context);
    return -ENOENT;
  }

  if (request->info_type == SMB2_0_INFO_FILESYSTEM)
  {
    if (ShouldPassthroughFileSystemInfo(request->file_info_class))  reply->output_buffer_length = FillFileSystemInfoRaw(request->file_info_class, (uint8_t**)&reply->output_buffer);
    else                                                            reply->output_buffer_length = FillNodeInfo(node, handle, request->info_type, request->file_info_class, &reply->output_buffer);

    return reply->output_buffer_length > 0 ? 0 : -EINVAL;
  }

  reply->output_buffer_length = FillNodeInfo(node, handle, request->info_type, request->file_info_class, &reply->output_buffer);
  return reply->output_buffer_length > 0 ? 0 : -EINVAL;
}

static int BootstrapContext(struct SombreroCore* core, struct smb2_context* context)
{
  struct connect_data* data;

  fprintf(stderr, "Sombrero[%p]: bootstrap begin core=%p\n", (void*)context, (void*)core);
  InitializeServer(&core->server);

  if ((data = (struct connect_data*)calloc(1, sizeof(struct connect_data))) == NULL)
  {
    smb2_set_error(context, "Failed to allocate connect_data");
    return -ENOMEM;
  }

  data->server_context       = &core->server;
  context->owning_server     = &core->server;
  context->connect_data      = data;
  context->max_transact_size = core->server.max_transact_size;
  context->max_read_size     = core->server.max_read_size;
  context->max_write_size    = core->server.max_write_size;
  fprintf(stderr, "Sombrero[%p]: bootstrap server=%p max=(%u,%u,%u)\n",
          (void*)context,
          (void*)&core->server,
          context->max_transact_size,
          context->max_read_size,
          context->max_write_size);

  if ((context->pdu = smb2_allocate_pdu(context, SMB2_NEGOTIATE, HandleNegotiateRequest, data)) == NULL)
  {
    smb2_set_error(context, "can not alloc pdu for request");
    FreeConnectData(context, data);
    return -ENOMEM;
  }

  TraceState("bootstrap:ready", context);
  return 0;
}

void PrepareSombreroCore(struct SombreroCore* core, HandleSombreroContextFunction function, void* closure)
{
  if (core != NULL)
  {
    core->closure         = closure;
    core->function        = function;
    core->server.handlers = &core->handlers;
    core->handlers = (struct smb2_server_request_handlers)
    {
      HandleDestroyPipeServer,
      HandleAuthorizePipeServer,
      HandlePipeSessionEstablished,
      NULL,
      HandlePipeTreeConnect,
      HandlePipeTreeDisconnect,
      HandlePipeCreate,
      HandlePipeClose,
      HandlePipeFlush,
      HandlePipeRead,
      HandlePipeWrite,
      NULL,
      NULL,
      HandlePipeLock,
      HandlePipeIoctl,
      HandlePipeCancel,
      HandlePipeEcho,
      NULL,
      NULL,
      HandlePipeQueryInfo,
      NULL
    };
  }
}

void HandleSombreroAccept(struct smb2_context* context, void* closure)
{
  struct SombreroCore* core;
  struct SombreroSession* session;

  core = (struct SombreroCore*)closure;
  fprintf(stderr, "Sombrero[%p]: accept closure=%p core=%p\n", (void*)context, closure, (void*)core);

  if ((core    != NULL) &&
      (context != NULL))
  {
    if ((core->ring == NULL) ||
        ((session = (struct SombreroSession*)calloc(1, sizeof(struct SombreroSession))) == NULL))
    {
      smb2_set_error(context, "failed to allocate sombrero session");
      smb2_close_context(context);
      return;
    }

    session->next_handle_id = 1U;
    AttachSambarOpaque(&session->sambar, core->ring, context, 1);
    smb2_register_error_callback(context, HandleSombreroError);

    if (BootstrapContext(core, context) == 0)
    {
      if (core->function != NULL)  core->function(core, context, core->closure);
      return;
    }

    smb2_close_context(context);
  }
}

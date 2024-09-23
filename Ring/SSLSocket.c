#include "SSLSocket.h"

// Important note about handling OpenSSL's error queue:
// https://github.com/openssl/openssl/issues/8470#issuecomment-550973323
// https://stackoverflow.com/questions/18179128/how-to-manage-the-error-queue-in-openssl-ssl-get-error-and-err-get-error
// https://www.arangodb.com/2014/07/started-hate-openssl/

#include <unistd.h>
#include <malloc.h>
#include <string.h>
#include <errno.h>

#include <openssl/err.h>

static void AppendBuffer(struct SSLSocket* socket, const void* data, size_t length)
{
  size_t size;

  size = socket->length + length;

  if (size > socket->size)
  {
    socket->size   = size;
    socket->buffer = (uint8_t*)realloc(socket->buffer, socket->size);
  }

  memcpy(socket->buffer + socket->length, data, length);
  socket->length = size;
}

static int TransmitPendingData(struct SSLSocket* socket)
{
  int result;
  uint8_t* buffer;

  buffer = socket->buffer;

  do
  {
    // In case of retry SSL_write should point to the same data
    // (thanks SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER) with the same length (which is <batch>)
    socket->batch += socket->length * (socket->batch == 0);
    result         = SSL_write(socket->connection, buffer, socket->batch);

    if (result > 0)
    {
      buffer         += result;
      socket->length -= result;
      socket->batch   = 0;
    }
  }
  while ((result > 0) &&
         (socket->length > 0));

  if ((socket->length > 0) &&
      (socket->buffer != buffer))
  {
    // Move the rest of data to the beginning of buffer to fit new data to the end fo buffer
    memmove(socket->buffer, buffer, socket->length);
  }

  return result;
}

static void HandleIOResult(struct SSLSocket* socket, int result, uint32_t flag)
{
  switch (SSL_get_error(socket->connection, result))
  {
    case SSL_ERROR_WANT_CONNECT:
    case SSL_ERROR_WANT_ACCEPT:
      socket->state &= ~SSL_FLAG_ACTIVE;
      break;

    case SSL_ERROR_WANT_WRITE:
      socket->state |= SSL_FLAG_WRITE;
      break;

    case SSL_ERROR_WANT_READ:
      socket->state |= SSL_FLAG_READ;
      break;

    case SSL_ERROR_NONE:
      break;

    case SSL_ERROR_ZERO_RETURN:
      socket->state &= ~SSL_FLAG_ACTIVE;
      socket->function(socket->closure, socket->connection, SSL_EVENT_DISCONNECTED, 0, NULL);
      break;

    case SSL_ERROR_SSL:
      // if (ERR_GET_REASON(error) == SSL_R_UNEXPECTED_EOF_WHILE_READING)
      // The SSL_ERROR_SYSCALL with errno value of 0 indicates unexpected EOF from the peer
      // This will be properly reported as SSL_ERROR_SSL with reason code SSL_R_UNEXPECTED_EOF_WHILE_READING in the OpenSSL 3.0
      socket->state &= ~SSL_FLAG_ACTIVE;
      socket->function(socket->closure, socket->connection, SSL_EVENT_FAILED, ERR_get_error(), NULL);
      break;

   case SSL_ERROR_SYSCALL:
      socket->state |= flag;
      break;
  }
}

static int HandleIOAction(struct SSLSocket* socket, int action)
{
  int result;
  struct FastRingDescriptor* descriptor;

  switch (action)
  {
    case SSL_ERROR_WANT_CONNECT:
      result = SSL_connect(socket->connection);
      HandleIOResult(socket, result, 0);
      break;

    case SSL_ERROR_WANT_ACCEPT:
      result = SSL_accept(socket->connection);
      HandleIOResult(socket, result, 0);
      break;

    default:
      result = -1;
      break;
  }

  if ((result > 0) &&
      (~socket->state & SSL_FLAG_ACTIVE) &&
      (~socket->state & SSL_FLAG_REMOVE))
  {
    socket->state |= SSL_FLAG_ACTIVE;
    socket->function(socket->closure, socket->connection, SSL_EVENT_CONNECTED, 0, NULL);
  }

  return result;
}

static void HandleBIOEvent(struct FastBIO* engine, int event, int parameter)
{
  int result;
  struct SSLSocket* socket;

  socket         = (struct SSLSocket*)engine->closure;
  socket->state |= SSL_FLAG_ENTER;

  if ((event & POLLERR) ||
      (event & POLLHUP))
  {
    socket->function(socket->closure, socket->connection, SSL_EVENT_DISCONNECTED, parameter, NULL);
    goto Final;
  }

  do
  {
    if ((~socket->state & SSL_FLAG_ACTIVE) &&
        (~socket->state & SSL_FLAG_REMOVE))
    {
      // SSL_ERROR_WANT_CONNECT or SSL_ERROR_WANT_ACCEPT
      HandleIOAction(socket, socket->role);
    }

    if (((engine->inbound.length != 0) ||
         (socket->state & SSL_FLAG_READ))  &&
        ( socket->state & SSL_FLAG_ACTIVE) &&
        (~socket->state & SSL_FLAG_REMOVE))
    {
      socket->state &= ~SSL_FLAG_READ;
      result         = socket->function(socket->closure, socket->connection, SSL_EVENT_RECEIVED, 0, NULL);
      HandleIOResult(socket, result, 0);
    }

    if (((socket->length != 0) ||
         (socket->state & SSL_FLAG_WRITE)) &&
        ( socket->state & SSL_FLAG_ACTIVE) &&
        (~socket->state & SSL_FLAG_REMOVE))
    {
      socket->state &= ~SSL_FLAG_WRITE;
      result         = TransmitPendingData(socket);
      HandleIOResult(socket, result, SSL_FLAG_WRITE);
    }
  }
  while (( socket->state & SSL_FLAG_ACTIVE)      &&
         (~socket->state & SSL_FLAG_REMOVE)      &&
         (~engine->outbound.condition & POLLOUT) &&
         ((engine->inbound.length          != 0) ||
          (socket->length                  != 0)));

  Final:

  socket->state ^= SSL_FLAG_ENTER;

  if (socket->state & SSL_FLAG_REMOVE)
  {
    // Release socket object safely
    ReleaseSSLSocket(socket);
  }
}

static int HandleVerifyRequest(int condition, X509_STORE_CTX* context)
{
  SSL* connection;
  struct SSLSocket* socket;

  if ((connection = (SSL*)X509_STORE_CTX_get_ex_data(context, SSL_get_ex_data_X509_STORE_CTX_idx())) &&
      (socket     = (struct SSLSocket*)SSL_get_app_data(connection)))
  {
    // Function can be called after destruction
    return socket->function(socket->closure, connection, SSL_EVENT_GREETED, condition, context);
  }

  return 0;
}

struct SSLSocket* CreateSSLSocket(struct FastRing* ring, struct FastRingBufferProvider* provider, struct FastBufferPool* inbound, struct FastBufferPool* outbound, SSL_CTX* context, int handle, int role, int option, uint32_t granularity, uint32_t limit, HandleSSLSocketEventFunction function, void* closure)
{
  BIO* engine;
  struct SSLSocket* socket;

  socket = (struct SSLSocket*)calloc(1, sizeof(struct SSLSocket));
  engine = CreateFastBIO(ring, provider, inbound, outbound, handle, granularity, limit, HandleBIOEvent, socket);

  socket->connection = SSL_new(context);
  socket->function   = function;
  socket->closure    = closure;
  socket->role       = role;

  SSL_set_bio(socket->connection, engine, engine);
  SSL_set_mode(socket->connection, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  SSL_set_options(socket->connection, SSL_OP_IGNORE_UNEXPECTED_EOF);
  SSL_set_read_ahead(socket->connection, 1);

  if (option >= SSL_VERIFY_NONE)
  {
    SSL_set_app_data(socket->connection, socket);
    SSL_set_verify(socket->connection, option, HandleVerifyRequest);
  }

  ERR_clear_error();
  HandleIOAction(socket, role);

  return socket;
}

int TransmitSSLSocketData(struct SSLSocket* socket, const void* data, size_t length)
{
  int result;
  BIO* engine;
  const uint8_t* buffer;
  struct FastRingDescriptor* descriptor;

  if ((socket->length != 0) ||
      (~socket->state & SSL_FLAG_ACTIVE))
  {
    AppendBuffer(socket, data, length);
    goto Final;
  }

  if (~socket->state & SSL_FLAG_ENTER)
  {
    // Clear errors only when called out of HandleIOEvent()
    ERR_clear_error();
  }

  do
  {
    result = SSL_write(socket->connection, data, length);

    if (result > 0)
    {
      data   += result;
      length -= result;
    }
  }
  while ((result > 0) &&
         (length > 0));

  if (result <= 0)
  {
    // Keep the data and its length (SSL_write specific)
    // please see TransmitPendingData() for the details
    socket->batch = length;
    AppendBuffer(socket, data, length);
  }

  HandleIOResult(socket, result, SSL_FLAG_WRITE);

  Final:

  if ((~socket->state & SSL_FLAG_ENTER) &&
      ((socket->state & SSL_FLAG_WRITE) ||
       (socket->state & SSL_FLAG_READ)  ||
       (socket->length != 0)))
  {
    engine = SSL_get_wbio(socket->connection);
    BIO_ctrl(engine, FASTBIO_CTRL_TOUCH, 0, NULL);
  }

  return result;
}

void ReleaseSSLSocket(struct SSLSocket* socket)
{
  if (socket != NULL)
  {
    if (socket->state & SSL_FLAG_ENTER)
    {
      socket->state |= SSL_FLAG_REMOVE;
      return;
    }

    SSL_set_app_data(socket->connection, NULL);
    SSL_shutdown(socket->connection);
    SSL_free(socket->connection);

    free(socket->buffer);
    free(socket);
  }
}

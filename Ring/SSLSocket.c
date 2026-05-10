#include "SSLSocket.h"

#include <errno.h>
#include <malloc.h>
#include <string.h>
#include <openssl/err.h>

#define BUFFER_GRANULARITY  2048

_Static_assert((BUFFER_GRANULARITY & (BUFFER_GRANULARITY - 1)) == 0, "BUFFER_GRANULARITY must be power of two");

static int CallEventFunction(struct SSLSocket* socket, int event, int parameter1, void* parameter2)
{
  if (socket->function != NULL)
  {
    // Function can be not installed yet or already detached
    return socket->function(socket->closure, socket->connection, event, parameter1, parameter2);
  }

  return -1;
}

static int AppendBuffer(struct SSLSocket* socket, const void* data, size_t length)
{
  size_t size;
  uint8_t* buffer;

  size = socket->length + length;

  if (size > socket->size)
  {
    size   = (size + BUFFER_GRANULARITY - 1) & ~(BUFFER_GRANULARITY - 1);
    buffer = (uint8_t*)realloc(socket->buffer, size);

    if (buffer == NULL)
    {
      errno = ENOMEM;
      return -1;
    }

    socket->size   = size;
    socket->buffer = buffer;
  }

  memcpy(socket->buffer + socket->length, data, length);
  socket->length += length;

  return 0;
}

static int TransmitPendingData(struct SSLSocket* socket)
{
  int count;
  int result;
  uint8_t* buffer;

  buffer = socket->buffer;

  do
  {
    // In case of retry SSL_write should point to the same data
    // (thanks SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER) with the same length (which is <batch>)
    count         = socket->length <= INT_MAX ? socket->length : INT_MAX;
    socket->batch = socket->batch  != 0       ? socket->batch  : count;

    result = SSL_write(socket->connection, buffer, socket->batch);

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
    // Move remaining data to the beginning of the buffer to fit new data at the end
    memmove(socket->buffer, buffer, socket->length);
  }

  return result;
}

static int HandleIOResult(struct SSLSocket* socket, int result, uint32_t flag)
{
  switch (SSL_get_error(socket->connection, result))
  {
    case SSL_ERROR_WANT_CONNECT:
    case SSL_ERROR_WANT_ACCEPT:
      socket->state &= ~SSL_FLAG_ACTIVE;
      break;

    case SSL_ERROR_WANT_WRITE:
    case SSL_ERROR_WANT_READ:
    case SSL_ERROR_SYSCALL:
      socket->state |= flag;
      break;

    case SSL_ERROR_SSL:
      socket->state &= ~SSL_FLAG_ACTIVE;
      CallEventFunction(socket, SSL_EVENT_FAILED, ERR_get_error(), NULL);
      return -1;

    case SSL_ERROR_ZERO_RETURN:
      socket->state &= ~SSL_FLAG_ACTIVE;
      CallEventFunction(socket, SSL_EVENT_DISCONNECTED, 0, NULL);
      return -1;
  }

  return 0;
}

static int HandleIOAction(struct SSLSocket* socket, int action)
{
  int result;
  BIO* engine;

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
    engine         = SSL_get_rbio(socket->connection);
    BIO_ctrl(engine, FASTBIO_CTRL_ENSURE, 0, NULL);
    CallEventFunction(socket, SSL_EVENT_CONNECTED, 0, NULL);
  }

  return result;
}

static void HandleBIOEvent(struct FastBIO* engine, int event, int parameter)
{
  int result;
  struct SSLSocket* socket;

  socket         = (struct SSLSocket*)engine->closure;
  socket->state |= SSL_FLAG_ENTER;

  ERR_clear_error();

  if ((event & POLLERR) ||
      (event & POLLHUP))
  {
    CallEventFunction(socket, SSL_EVENT_DISCONNECTED, parameter, NULL);
    goto Final;
  }

  do
  {
    engine->flags &= ~FASTBIO_FLAG_POLL_PROGRESS;

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
      result         = CallEventFunction(socket, SSL_EVENT_RECEIVED, 0, NULL);
      HandleIOResult(socket, result, SSL_FLAG_READ);
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
          (socket->length                  != 0) ||
          (engine->flags & FASTBIO_FLAG_POLL_PROGRESS)));

  if ((event & POLLOUT) &&
      (socket->length        == 0)       &&
      (engine->outbound.head == NULL)    &&
      ( socket->state & SSL_FLAG_ACTIVE) &&
      (~socket->state & SSL_FLAG_REMOVE))
  {
    // Socket is ready to accept more data from the owner
    CallEventFunction(socket, SSL_EVENT_DRAINED, 0, NULL);
  }

  Final:

  socket->state &= ~SSL_FLAG_ENTER;

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
    return CallEventFunction(socket, SSL_EVENT_GREETED, condition, context);
  }

  return 0;
}

struct SSLSocket* CreateSSLSocket(struct FastRing* ring, struct FastRingBufferProvider* provider, struct FastBufferPool* inbound, struct FastBufferPool* outbound, SSL_CTX* context, int handle, int role, int option, uint32_t granularity, uint32_t limit, HandleSSLSocketEventFunction function, void* closure)
{
  BIO* engine;
  struct SSLSocket* socket;

  socket = NULL;
  engine = NULL;

  if ((socket = (struct SSLSocket*)calloc(1, sizeof(struct SSLSocket))) &&
      (engine = CreateFastBIO(ring, provider, inbound, outbound, handle, option & SSL_OPTION_OP_MASK, granularity, limit, HandleBIOEvent, socket)) &&
      (socket->connection = SSL_new(context)))
  {
    SSL_set_bio(socket->connection, engine, engine);
    SSL_set_mode(socket->connection, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_set_options(socket->connection, SSL_OP_IGNORE_UNEXPECTED_EOF | option & SSL_OPTION_OP_MASK);
    SSL_set_read_ahead(socket->connection, 1);

    if (option & SSL_OPTION_VERIFY_MASK)
    {
      SSL_set_app_data(socket->connection, socket);
      SSL_set_verify(socket->connection, option & SSL_OPTION_VERIFY_MASK, HandleVerifyRequest);
    }

    socket->function = function;
    socket->closure  = closure;
    socket->role     = role;

    ERR_clear_error();
    BIO_ctrl(engine, FASTBIO_CTRL_TOUCH, 0, NULL);

    return socket;
  }

  BIO_free(engine);
  free(socket);
  return NULL;
}

int TransmitSSLSocketData(struct SSLSocket* socket, const void* data, size_t length)
{
  int count;
  int result;
  BIO* engine;

  if ((socket->length != 0) ||
      (~socket->state & SSL_FLAG_ACTIVE))
  {
    result = AppendBuffer(socket, data, length);
    goto Final;
  }

  if (~socket->state & SSL_FLAG_ENTER)
  {
    // Clear errors only when called out of HandleIOEvent()
    ERR_clear_error();
  }

  do
  {
    count  = length < INT_MAX ? length : INT_MAX;
    result = SSL_write(socket->connection, data, count);

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
    // Preserve the failed SSL_write() length for retry; see TransmitPendingData()
    socket->batch = count;

    if (AppendBuffer(socket, data, length) < 0)
    {
      socket->batch = 0;
      result        = -1;
      goto Final;
    }
  }

  if (HandleIOResult(socket, result, SSL_FLAG_WRITE) == 0)
  {
    Final:

    if ((~socket->state & SSL_FLAG_ENTER) &&
        ((socket->state & SSL_FLAG_WRITE) ||
         (socket->state & SSL_FLAG_READ)  ||
         (socket->length != 0)))
    {
      engine = SSL_get_wbio(socket->connection);
      BIO_ctrl(engine, FASTBIO_CTRL_TOUCH, 0, NULL);
    }
  }

  return result;
}

void ReleaseSSLSocket(struct SSLSocket* socket)
{
  if (socket != NULL)
  {
    socket->function = NULL;
    socket->closure  = NULL;

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

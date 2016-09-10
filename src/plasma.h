#ifndef PLASMA_H
#define PLASMA_H

#include <inttypes.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#ifdef NDEBUG
  #define LOG_DEBUG(M, ...)
#else
  #define LOG_DEBUG(M, ...) \
    fprintf(stderr, "[DEBUG] (%s:%d) " M "\n", __FILE__, __LINE__, ##__VA_ARGS__)
#endif

#define LOG_ERR(M, ...) \
  fprintf(stderr, "[ERROR] (%s:%d: errno: %s) " M "\n", \
    __FILE__, __LINE__, errno == 0 ? "None" : strerror(errno), ##__VA_ARGS__)

#define LOG_INFO(M, ...) \
  fprintf(stderr, "[INFO] (%s:%d) " M "\n", __FILE__, __LINE__, ##__VA_ARGS__)

#define LOG_REDIS_ERR(context, M, ...) \
  fprintf(stderr, "[ERROR] (%s:%d: message: %s) " M "\n", \
    __FILE__, __LINE__, context->errstr, ##__VA_ARGS__)

typedef struct {
  int64_t size;
  int64_t create_time;
  int64_t construct_duration;
} plasma_object_info;

/* Size of object ids */
#define PLASMA_SHA1_SIZE 20

/* Represents an object id hash, can hold a full SHA1 hash */
typedef struct {
  unsigned char id[PLASMA_SHA1_SIZE];
} plasma_id;

enum plasma_request_type {
  /* Create a new object. */
  PLASMA_CREATE,
  /* Get an object. */
  PLASMA_GET,
  /* seal an object. */
  PLASMA_SEAL,
  /* request transfer to another store. */
  PLASMA_TRANSFER,
  /* Header for sending data. */
  PLASMA_DATA,
  /* Put object id into the object table. */
  PLASMA_LINK
};

typedef struct {
  uint8_t addr[4];
  int port;
} plasma_addr;

// TODO(pcm): make this platform independent, see http://stackoverflow.com/a/8001279
typedef struct {
  int type;
  plasma_id object_id;
  int64_t size;
  plasma_addr addr;
} plasma_request;

enum plasma_reply_type {
  /* the file descriptor represents an object */
  PLASMA_OBJECT,
  /* the file descriptor represents a future */
  PLASMA_FUTURE,
};

typedef struct {
  int type;
  int64_t size;
} plasma_reply;

typedef struct {
  plasma_id object_id;
  void *data;
  int64_t size;
  int writable;
} plasma_buffer;

/* Convert a 20 byte sha1 hash to a hexdecimal string. This function assumes
 * that buffer points to an already allocated char array of size 2 * PLASMA_SHA1_SIZE + 1 */
char *sha1_to_hex(const unsigned char *sha1, char *buffer);

/* Connect to the local plasma store UNIX domain socket */
int plasma_store_connect(const char* socket_name);

/* Connect to a possibly remote plasma manager */
int plasma_manager_connect(const char* addr, int port);

void plasma_create(int store, plasma_id object_id, int64_t size, void **data);
void plasma_get(int store, plasma_id object_id, int64_t *size, void **data);
void plasma_seal(int store, plasma_id object_id);

void plasma_send(int conn, plasma_request *req);

#endif

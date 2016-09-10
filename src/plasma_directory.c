#include "plasma_directory.h"

static void poll_add_read(void *privdata) {
  plasma_directory *directory = (plasma_directory*) privdata;
  if (!directory->reading) {
    directory->reading = 1;
    event_loop_get(directory->loop, 0)->events |= POLLIN;
  }
}

static void poll_del_read(void *privdata) {
  plasma_directory *directory = (plasma_directory*) privdata;
  if (directory->reading) {
    directory->reading = 0;
    event_loop_get(directory->loop, 0)->events &= ~POLLIN;
  }
}

static void poll_add_write(void *privdata) {
  plasma_directory *directory = (plasma_directory*) privdata;
  if (!directory->writing) {
    directory->writing = 1;
    event_loop_get(directory->loop, 0)->events |= POLLOUT;
  }
}

static void poll_del_write(void *privdata) {
  plasma_directory *directory = (plasma_directory*) privdata;
  if (directory->writing) {
    directory->writing = 0;
    event_loop_get(directory->loop, 0)->events &= ~POLLOUT;
  }
}

#define PLASMA_CHECK_REDIS_CONNECT(CONTEXT_TYPE, context, M, ...) \
  do { \
    CONTEXT_TYPE *_context = (context); \
    if (!_context) { \
      LOG_ERR("could not allocate redis context"); \
      exit(-1); \
    } \
    if (_context->err) { \
      LOG_REDIS_ERR(_context, M, ##__VA_ARGS__); \
      exit(-1); \
    } \
  } while (0);

void plasma_directory_link(plasma_directory* directory, plasma_id object_id) {
  static char hex_object_id[2 * PLASMA_SHA1_SIZE + 1];
  sha1_to_hex(&object_id.id[0], &hex_object_id[0]);
  printf("XXX %s\n", &hex_object_id[0]);
  redisAsyncCommand(directory->context, NULL, NULL, "SET test value");
  redisAsyncCommand(directory->context, NULL, NULL, "SADD obj:%s %d", &hex_object_id[0], directory->manager_id);
  if (directory->context->err) {
    LOG_REDIS_ERR(directory->context, "could not add directory link");
  }
}

void plasma_directory_init(plasma_directory* directory, const char* manager_address, int manager_port, const char* directory_address, int directory_port) {
  redisReply *reply;
  long long num_managers;
  /* First use the synchronous redis API to initialize the connection. */
  redisContext *context = redisConnect(directory_address, directory_port);
  PLASMA_CHECK_REDIS_CONNECT(redisContext, context, "could not connect to redis %s:%d", directory_address, directory_port);
  /* Add new client using optimistic locking. */
  while (1) {
    reply = redisCommand(context, "WATCH plasma_managers");
    freeReplyObject(reply);
    reply = redisCommand(context, "HLEN plasma_managers");
    num_managers = reply->integer;
    freeReplyObject(reply);
    reply = redisCommand(context, "MULTI");
    freeReplyObject(reply);
    reply = redisCommand(context, "HSET plasma_managers %lld %s:%d", num_managers, manager_address, manager_port);
    freeReplyObject(reply);
    reply = redisCommand(context, "EXEC");
    if (reply) {
      freeReplyObject(reply);
      break;
    }
    freeReplyObject(reply);
  }
  redisFree(context);

  directory->manager_id = num_managers;

  /* Now establish the asynchronous connection. */
  directory->context = redisAsyncConnect(directory_address, directory_port);
  PLASMA_CHECK_REDIS_CONNECT(redisAsyncContext, directory->context, "could not connect to redis %s:%d", directory_address, directory_port);
  directory->context->data = directory;
}

void plasma_directory_event(plasma_directory *directory) {
  if (directory->reading) {
    redisAsyncHandleRead(directory->context);
  }
  if (directory->writing) {
    redisAsyncHandleWrite(directory->context);
  }
}

int plasma_directory_attach(plasma_directory *directory, event_loop *loop) {
  directory->loop = loop;
  
  redisAsyncContext *ac = directory->context;
  redisContext *c = &(ac->c);
  
  if (ac->ev.data != NULL) {
    return REDIS_ERR;
  }
  
  ac->ev.addRead = poll_add_read;
  ac->ev.delRead = poll_del_read;
  ac->ev.addWrite = poll_add_write;
  ac->ev.delWrite = poll_del_write;
  // TODO(pcm): Implement cleanup function
  
  ac->ev.data = directory;
  
  return c->fd;
}

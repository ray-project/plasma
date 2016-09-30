/* PLASMA STORE: This is a simple object store server process
 *
 * It accepts incoming client connections on a unix domain socket
 * (name passed in via the -s option of the executable) and uses a
 * single thread to serve the clients. Each client establishes a
 * connection and can create objects, wait for objects and seal
 * objects through that connection.
 *
 * It keeps a hash table that maps object_ids (which are 20 byte long,
 * just enough to store and SHA1 hash) to memory mapped files. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <getopt.h>
#include <string.h>
#include <limits.h>
#include <poll.h>

#include "common.h"
#include "event_loop.h"
#include "io.h"
#include "uthash.h"
#include "fling.h"
#include "malloc.h"
#include "plasma.h"

#define MAX_NUM_CLIENTS 100

void* dlmalloc(size_t);
void dlfree(void*);

void plasma_send_reply(int fd, plasma_reply* reply) {
  int reply_count = sizeof(plasma_reply);
  if (write(fd, reply, reply_count) != reply_count) {
    LOG_ERR("write error, fd = %d", fd);
    exit(-1);
  }
}

typedef struct {
  /* Object id of this object. */
  plasma_id object_id;
  /* Object info like size, creation time and owner. */
  plasma_object_info info;
  /* Memory mapped file containing the object. */
  int fd;
  /* Size of the underlying map. */
  int64_t map_size;
  /* Offset from the base of the mmap. */
  ptrdiff_t offset;
  /* Handle for the uthash table. */
  UT_hash_handle handle;
  /* Pointer to the object data. Needed to free the object. */
  uint8_t* pointer;
} object_table_entry;

/* Objects that are still being written by their owner process. */
object_table_entry* open_objects = NULL;

/* Objects that have already been sealed by their owner process and
 * can now be shared with other processes. */
object_table_entry* sealed_objects = NULL;

typedef struct {
  /* Object id of this object. */
  plasma_id object_id;
  /* Number of processes waiting for the object. */
  int num_waiting;
  /* Socket connections to waiting clients. */
  int conn[MAX_NUM_CLIENTS];
  /* Handle for the uthash table. */
  UT_hash_handle handle;
} object_notify_entry;

/* Objects that processes are waiting for. */
object_notify_entry* objects_notify = NULL;

/* Create a new object buffer in the hash table. */
void create_object(int conn, plasma_request* req) {
  LOG_INFO("creating object"); /* TODO(pcm): add object_id here */

  object_table_entry* entry;
  HASH_FIND(handle, open_objects, &req->object_id, sizeof(plasma_id), entry);
  CHECKM(entry == NULL, "Cannot create object twice.");

  uint8_t* pointer = dlmalloc(req->data_size + req->metadata_size);
  int fd;
  int64_t map_size;
  ptrdiff_t offset;
  get_malloc_mapinfo(pointer, &fd, &map_size, &offset);
  assert(fd != -1);

  entry = malloc(sizeof(object_table_entry));
  memcpy(&entry->object_id, &req->object_id, 20);
  entry->info.data_size = req->data_size;
  entry->info.metadata_size = req->metadata_size;
  entry->pointer = pointer;
  /* TODO(pcm): set the other fields */
  entry->fd = fd;
  entry->map_size = map_size;
  entry->offset = offset;
  HASH_ADD(handle, open_objects, object_id, sizeof(plasma_id), entry);
  plasma_reply reply;
  memset(&reply, 0, sizeof(reply));
  reply.data_offset = offset;
  reply.metadata_offset = offset + req->data_size;
  reply.map_size = map_size;
  reply.data_size = req->data_size;
  reply.metadata_size = req->metadata_size;
  reply.store_fd_val = fd;
  send_fd(conn, fd, (char*) &reply, sizeof(reply));
}

/* Get an object from the hash table. */
void get_object(int conn, plasma_request* req) {
  object_table_entry* entry;
  HASH_FIND(handle, sealed_objects, &req->object_id, sizeof(plasma_id), entry);
  if (entry) {
    plasma_reply reply;
    memset(&reply, 0, sizeof(plasma_reply));
    reply.data_offset = entry->offset;
    reply.map_size = entry->map_size;
    reply.data_size = entry->info.data_size;
    reply.metadata_size = entry->info.metadata_size;
    reply.store_fd_val = entry->fd;
    send_fd(conn, entry->fd, (char*) &reply, sizeof(plasma_reply));
  } else {
    object_notify_entry* notify_entry;
    LOG_INFO("object not in hash table of sealed objects");
    HASH_FIND(handle, objects_notify, &req->object_id, sizeof(plasma_id),
              notify_entry);
    if (!notify_entry) {
      notify_entry = malloc(sizeof(object_notify_entry));
      memset(notify_entry, 0, sizeof(object_notify_entry));
      notify_entry->num_waiting = 0;
      memcpy(&notify_entry->object_id, &req->object_id, 20);
      HASH_ADD(handle, objects_notify, object_id, sizeof(plasma_id),
               notify_entry);
    }
    CHECKM(notify_entry->num_waiting < MAX_NUM_CLIENTS - 1,
           "This exceeds the maximum number of clients.");
    notify_entry->conn[notify_entry->num_waiting] = conn;
    notify_entry->num_waiting += 1;
  }
}

/* Check if an object is present. */
void check_if_object_present(int conn, plasma_request* req) {
  object_table_entry* entry;
  HASH_FIND(handle, sealed_objects, &req->object_id, sizeof(plasma_id), entry);
  plasma_reply reply;
  memset(&reply, 0, sizeof(plasma_reply));
  reply.has_object = entry ? 1 : 0;
  plasma_send_reply(conn, &reply);
}

/* Seal an object that has been created in the hash table. */
void seal_object(int conn, plasma_request* req) {
  LOG_INFO("sealing object");  // TODO(pcm): add object_id here
  object_table_entry* entry;
  HASH_FIND(handle, open_objects, &req->object_id, sizeof(plasma_id), entry);
  if (!entry) {
    return; /* TODO(pcm): return error */
  }
  int fd = entry->fd;
  HASH_DELETE(handle, open_objects, entry);
  HASH_ADD(handle, sealed_objects, object_id, sizeof(plasma_id), entry);
  /* Inform processes that the object is ready now. */
  object_notify_entry* notify_entry;
  HASH_FIND(handle, objects_notify, &req->object_id, sizeof(plasma_id),
            notify_entry);
  if (!notify_entry) {
    return;
  }
  plasma_reply reply = {.data_offset = entry->offset,
                        .map_size = entry->map_size,
                        .data_size = entry->info.data_size,
                        .metadata_size = entry->info.metadata_size,
                        .store_fd_val = fd};
  for (int i = 0; i < notify_entry->num_waiting; ++i) {
    send_fd(notify_entry->conn[i], entry->fd, (char*) &reply,
            sizeof(plasma_reply));
  }
  HASH_DELETE(handle, objects_notify, notify_entry);
  free(notify_entry);
}

/* Delete an object that has been created in the hash table. */
void delete_object(int conn, plasma_request* req) {
  LOG_INFO("deleting object");  // TODO(rkn): add object_id here
  object_table_entry* entry;
  HASH_FIND(handle, sealed_objects, &req->object_id, sizeof(plasma_id), entry);
  /* TODO(rkn): This should probably not fail, but should instead throw an
   * error. Maybe we should also support deleting objects that have been created
   * but not sealed. */
  CHECKM(entry != NULL, "To delete an object it must have been sealed.");
  uint8_t* pointer = entry->pointer;
  HASH_DELETE(handle, sealed_objects, entry);
  dlfree(pointer);
}

void process_message(event_loop* loop,
                     int client_sock,
                     void* context,
                     int events) {
  int64_t type;
  int64_t length;
  plasma_request* req;
  read_message(client_sock, &type, &length, (uint8_t**) &req);

  switch (type) {
  case PLASMA_CREATE:
    create_object(client_sock, req);
    break;
  case PLASMA_GET:
    get_object(client_sock, req);
    break;
  case PLASMA_CONTAINS:
    check_if_object_present(client_sock, req);
    break;
  case PLASMA_SEAL:
    seal_object(client_sock, req);
    break;
  case PLASMA_DELETE:
    delete_object(client_sock, req);
    break;
  case DISCONNECT_CLIENT: {
    LOG_INFO("Disconnecting client on fd %d", client_sock);
    event_loop_remove_file(loop, client_sock);
  } break;
  default:
    /* This code should be unreachable. */
    CHECK(0);
  }
  free(req);
}

void new_client_connection(event_loop* loop,
                           int listener_sock,
                           void* context,
                           int events) {
  int new_socket = accept_client(listener_sock);
  event_loop_add_file(loop, new_socket, EVENT_LOOP_READ, process_message,
                      context);
  LOG_INFO("new connection with fd %d", new_socket);
}

void start_server(char* socket_name) {
  int socket = bind_ipc_sock(socket_name);
  CHECK(socket >= 0);
  event_loop* loop = event_loop_create();
  event_loop_add_file(loop, socket, EVENT_LOOP_READ, new_client_connection,
                      NULL);
  event_loop_run(loop);
}

int main(int argc, char* argv[]) {
  char* socket_name = NULL;
  int c;
  while ((c = getopt(argc, argv, "s:")) != -1) {
    switch (c) {
    case 's':
      socket_name = optarg;
      break;
    default:
      exit(-1);
    }
  }
  if (!socket_name) {
    LOG_ERR("please specify socket for incoming connections with -s switch");
    exit(-1);
  }
  LOG_INFO("starting server listening on %s", socket_name);
  start_server(socket_name);
}

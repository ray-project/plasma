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
#include <sys/types.h>
#include <sys/un.h>
#include <getopt.h>
#include <string.h>
#include <signal.h>
#include <limits.h>
#include <poll.h>

#include "common.h"
#include "event_loop.h"
#include "io.h"
#include "uthash.h"
#include "utarray.h"
#include "fling.h"
#include "malloc.h"
#include "plasma_store.h"

void *dlmalloc(size_t);
void dlfree(void *);

/**
 * This is used by the Plasma Store to send a reply to the Plasma Client.
 */
void plasma_send_reply(int fd, plasma_reply *reply) {
  int reply_count = sizeof(plasma_reply);
  if (write(fd, reply, reply_count) != reply_count) {
    LOG_ERR("write error, fd = %d", fd);
    exit(-1);
  }
}

typedef struct {
  /* Object id of this object. */
  object_id object_id;
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
  uint8_t *pointer;
  /** A hash table of the indices of the clients that are currently using this
   *  object. */
  UT_array *clients;
} object_table_entry;

typedef struct {
  /* Object id of this object. */
  object_id object_id;
  /* Indices of the waiting clients. */
  UT_array *waiting_client_indices;
  /* Handle for the uthash table. */
  UT_hash_handle handle;
} object_notify_entry;

/** Contains all information that is associated with a client. */
typedef struct {
  /** The socket used to communicate with the client. */
  int sock;
} client;

/* This is used to define the array of clients used to define the
 * object_table_entry type. */
UT_icd client_icd = {sizeof(client), NULL, NULL, NULL};

/* This is used to define the array of object IDs used to define the
 * notification_queue type. */
UT_icd object_table_entry_icd = {sizeof(object_id), NULL, NULL, NULL};

typedef struct {
  /** Client file descriptor. This is used as a key for the hash table. */
  int subscriber_fd;
  /** The object IDs to notify the client about. We notify the client about the
   *  IDs in the order that the objects were sealed. */
  UT_array *object_ids;
  /** Handle for the uthash table. */
  UT_hash_handle hh;
} notification_queue;

/** Mapping from the socket fd of a client to its client_index. Note that if a
 *  client disconnects, its file descriptor can be reused for a new client. */
typedef struct {
  /** The socket fd of a client. */
  int sock;
  /** The index of the client in plasma_store_state->clients. */
  int64_t client_index;
  /** Handle for the hash table. */
  UT_hash_handle hh;
} client_map_entry;

/* This is used to define arrays of client indices. */
UT_icd int64_icd = {sizeof(int64_t), NULL, NULL, NULL};

struct plasma_store_state {
  /* Event loop of the plasma store. */
  event_loop *loop;
  /* Objects that are still being written by their owner process. */
  object_table_entry *open_objects;
  /* Objects that have already been sealed by their owner process and
   * can now be shared with other processes. */
  object_table_entry *sealed_objects;
  /* Objects that processes are waiting for. */
  object_notify_entry *objects_notify;
  /** The pending notifications that have not been sent to subscribers because
   *  the socket send buffers were full. This is a hash table from client file
   *  descriptor to an array of object_ids to send to that client. */
  notification_queue *pending_notifications;
  /** List of clients available to this store. The index into this array is the
   *  client_index and is used to identify clients throughout the program. */
  UT_array *clients;
  /** A hash table mapping the file descriptor used for a client to the index of
   *  the client in the clients array. */
  client_map_entry *client_map;
};

plasma_store_state *init_plasma_store(event_loop *loop) {
  plasma_store_state *state = malloc(sizeof(plasma_store_state));
  state->loop = loop;
  state->open_objects = NULL;
  state->sealed_objects = NULL;
  state->objects_notify = NULL;
  state->pending_notifications = NULL;
  utarray_new(state->clients, &client_icd);
  state->client_map = NULL;
  return state;
}

/* If this client is not already using object, add the client index to the
 * object's list of clients, otherwise do nothing. */
void record_client_using_object(object_table_entry *entry,
                                int64_t client_index) {
  /* Check if this client is already using the object. */
  for (int64_t *ci = (int64_t *) utarray_front(entry->clients); ci != NULL;
       ci = (int64_t *) utarray_next(entry->clients, ci)) {
    if (*ci == client_index) {
      return;
    }
  }
  /* Add the client index to the list of clients using this object. */
  utarray_push_back(entry->clients, &client_index);
}

/* Create a new object buffer in the hash table. */
void create_object(plasma_store_state *plasma_state,
                   int64_t client_index,
                   object_id object_id,
                   int64_t data_size,
                   int64_t metadata_size,
                   plasma_object *result) {
  LOG_DEBUG("creating object"); /* TODO(pcm): add object_id here */

  object_table_entry *entry;
  HASH_FIND(handle, plasma_state->open_objects, &object_id, sizeof(object_id),
            entry);
  /* TODO(swang): Return this error to the client instead of
   * exiting. */
  CHECKM(entry == NULL, "Cannot create object twice.");

  uint8_t *pointer = dlmalloc(data_size + metadata_size);
  int fd;
  int64_t map_size;
  ptrdiff_t offset;
  get_malloc_mapinfo(pointer, &fd, &map_size, &offset);
  assert(fd != -1);

  entry = malloc(sizeof(object_table_entry));
  memcpy(&entry->object_id, &object_id, sizeof(object_id));
  entry->info.data_size = data_size;
  entry->info.metadata_size = metadata_size;
  entry->pointer = pointer;
  /* TODO(pcm): set the other fields */
  entry->fd = fd;
  entry->map_size = map_size;
  entry->offset = offset;
  utarray_new(entry->clients, &int64_icd);
  HASH_ADD(handle, plasma_state->open_objects, object_id, sizeof(object_id),
           entry);
  result->handle.store_fd = fd;
  result->handle.mmap_size = map_size;
  result->data_offset = offset;
  result->metadata_offset = offset + data_size;
  result->data_size = data_size;
  result->metadata_size = metadata_size;
  /* Record that this client is using this object. */
  record_client_using_object(entry, client_index);
}

/* Get an object from the hash table. */
int get_object(plasma_store_state *plasma_state,
               int64_t client_index,
               int conn,
               object_id object_id,
               plasma_object *result) {
  object_table_entry *entry;
  HASH_FIND(handle, plasma_state->sealed_objects, &object_id, sizeof(object_id),
            entry);
  if (entry) {
    result->handle.store_fd = entry->fd;
    result->handle.mmap_size = entry->map_size;
    result->data_offset = entry->offset;
    result->metadata_offset = entry->offset + entry->info.data_size;
    result->data_size = entry->info.data_size;
    result->metadata_size = entry->info.metadata_size;
    /* If necessary, record that this client is using this object. In the case
     * where entry == NULL, this will be called from seal_object. */
    record_client_using_object(entry, client_index);
    return OBJECT_FOUND;
  } else {
    object_notify_entry *notify_entry;
    LOG_DEBUG("object not in hash table of sealed objects");
    HASH_FIND(handle, plasma_state->objects_notify, &object_id,
              sizeof(object_id), notify_entry);
    if (!notify_entry) {
      notify_entry = malloc(sizeof(object_notify_entry));
      memset(notify_entry, 0, sizeof(object_notify_entry));
      utarray_new(notify_entry->waiting_client_indices, &int64_icd);
      memcpy(&notify_entry->object_id, &object_id, sizeof(object_id));
      HASH_ADD(handle, plasma_state->objects_notify, object_id,
               sizeof(object_id), notify_entry);
    }
    utarray_push_back(notify_entry->waiting_client_indices, &client_index);
  }
  return OBJECT_NOT_FOUND;
}

int remove_client_from_object_clients(object_table_entry *entry,
                                      int64_t client_index) {
  /* Find the location of the client index in the array. */
  int i = 0;
  for (int64_t *ci = (int64_t *) utarray_front(entry->clients); ci != NULL;
       ci = (int64_t *) utarray_next(entry->clients, ci)) {
    if (*ci == client_index) {
      break;
    }
    i += 1;
  }
  /* Check if the client index actually appeared in the array. */
  if (i < utarray_len(entry->clients)) {
    /* Remove the client index from the array. */
    utarray_erase(entry->clients, i, 1);
    /* Return 1 to indicate that the client index was removed. */
    return 1;
  }
  /* Return 0 to indicate that the client index was not removed. */
  return 0;
}

void release_object(plasma_store_state *plasma_state,
                    int64_t index,
                    object_id object_id) {
  object_table_entry *open_entry;
  object_table_entry *sealed_entry;

  HASH_FIND(handle, plasma_state->open_objects, &object_id, sizeof(object_id),
            open_entry);
  HASH_FIND(handle, plasma_state->sealed_objects, &object_id, sizeof(object_id),
            sealed_entry);
  /* Exactly one of open_entry and sealed_entry should be NULL. */
  CHECK((open_entry == NULL) != (sealed_entry == NULL));
  /* Remove the client index from the object's array of clients. */
  if (open_entry != NULL) {
    CHECK(remove_client_from_object_clients(open_entry, index) == 1);
  } else {
    CHECK(remove_client_from_object_clients(sealed_entry, index) == 1);
  }
}

/* Check if an object is present. */
int contains_object(plasma_store_state *plasma_state, object_id object_id) {
  object_table_entry *entry;
  HASH_FIND(handle, plasma_state->sealed_objects, &object_id, sizeof(object_id),
            entry);
  return entry ? OBJECT_FOUND : OBJECT_NOT_FOUND;
}

/* Seal an object that has been created in the hash table. */
void seal_object(plasma_store_state *plasma_state, object_id object_id) {
  LOG_DEBUG("sealing object");  // TODO(pcm): add object_id here
  object_table_entry *entry;
  HASH_FIND(handle, plasma_state->open_objects, &object_id, sizeof(object_id),
            entry);
  CHECK(entry != NULL);
  /* Move the object table entry from the table of open objects to the table of
   * sealed objects. */
  HASH_DELETE(handle, plasma_state->open_objects, entry);
  HASH_ADD(handle, plasma_state->sealed_objects, object_id, sizeof(object_id),
           entry);

  /* Inform all subscribers that a new object has been sealed. */
  notification_queue *queue, *temp_queue;
  HASH_ITER(hh, plasma_state->pending_notifications, queue, temp_queue) {
    utarray_push_back(queue->object_ids, &object_id);
    send_notifications(plasma_state->loop, queue->subscriber_fd, plasma_state,
                       0);
  }

  /* Inform processes getting this object that the object is ready now. */
  object_notify_entry *notify_entry;
  HASH_FIND(handle, plasma_state->objects_notify, &object_id, sizeof(object_id),
            notify_entry);
  if (notify_entry) {
    plasma_reply reply;
    memset(&reply, 0, sizeof(reply));
    plasma_object *result = &reply.object;
    result->handle.store_fd = entry->fd;
    result->handle.mmap_size = entry->map_size;
    result->data_offset = entry->offset;
    result->metadata_offset = entry->offset + entry->info.data_size;
    result->data_size = entry->info.data_size;
    result->metadata_size = entry->info.metadata_size;
    HASH_DELETE(handle, plasma_state->objects_notify, notify_entry);
    /* Send notifications to the clients that were waiting for this object. */
    for (int64_t *client_index =
             (int64_t *) utarray_front(notify_entry->waiting_client_indices);
         client_index != NULL;
         client_index = (int64_t *) utarray_next(
             notify_entry->waiting_client_indices, client_index)) {
      int client_socket = client_index_to_socket(plasma_state, *client_index);
      send_fd(client_socket, reply.object.handle.store_fd, (char *) &reply,
              sizeof(reply));
      /* Record that the client is using this object. */
      record_client_using_object(entry, *client_index);
    }
    utarray_free(notify_entry->waiting_client_indices);
    free(notify_entry);
  }
}

/* Delete an object that has been created in the hash table. */
void delete_object(plasma_store_state *plasma_state, object_id object_id) {
  LOG_DEBUG("deleting object");  // TODO(rkn): add object_id here
  object_table_entry *entry;
  HASH_FIND(handle, plasma_state->sealed_objects, &object_id, sizeof(object_id),
            entry);
  /* TODO(rkn): This should probably not fail, but should instead throw an
   * error. Maybe we should also support deleting objects that have been created
   * but not sealed. */
  CHECKM(entry != NULL, "To delete an object it must have been sealed.");
  uint8_t *pointer = entry->pointer;
  HASH_DELETE(handle, plasma_state->sealed_objects, entry);
  dlfree(pointer);
  utarray_free(entry->clients);
  free(entry);
}

/* Send more notifications to a subscriber. */
void send_notifications(event_loop *loop,
                        int client_sock,
                        void *context,
                        int events) {
  plasma_store_state *plasma_state = context;

  notification_queue *queue;
  HASH_FIND_INT(plasma_state->pending_notifications, &client_sock, queue);
  CHECK(queue != NULL);

  int num_processed = 0;
  /* Loop over the array of pending notifications and send as many of them as
   * possible. */
  for (object_id *obj_id = (object_id *) utarray_front(queue->object_ids);
       obj_id != NULL;
       obj_id = (object_id *) utarray_next(queue->object_ids, obj_id)) {
    /* Attempt to send a notification about this object ID. */
    int nbytes = send(client_sock, obj_id, sizeof(object_id), 0);
    if (nbytes >= 0) {
      CHECK(nbytes == sizeof(object_id));
    } else if (nbytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      LOG_DEBUG(
          "The socket's send buffer is full, so we are caching this "
          "notification and will send it later.");
      break;
    } else {
      CHECKM(0, "This code should be unreachable.");
    }
    num_processed += 1;
  }
  /* Remove the sent notifications from the array. */
  utarray_erase(queue->object_ids, 0, num_processed);
}

/* Subscribe to notifications about sealed objects. */
void subscribe_to_updates(plasma_store_state *plasma_state,
                          int64_t index,
                          int conn) {
  LOG_DEBUG("subscribing to updates");
  char dummy;
  int fd = recv_fd(conn, &dummy, 1);
  CHECKM(HASH_CNT(handle, plasma_state->open_objects) == 0,
         "plasma_subscribe should be called before any objects are created.");
  CHECKM(HASH_CNT(handle, plasma_state->sealed_objects) == 0,
         "plasma_subscribe should be called before any objects are created.");
  /* Create a new array to buffer notifications that can't be sent to the
   * subscriber yet because the socket send buffer is full. TODO(rkn): the queue
   * never gets freed. */
  notification_queue *queue =
      (notification_queue *) malloc(sizeof(notification_queue));
  queue->subscriber_fd = fd;
  utarray_new(queue->object_ids, &object_table_entry_icd);
  HASH_ADD_INT(plasma_state->pending_notifications, subscriber_fd, queue);
  /* Add a callback to the event loop to send queued notifications whenever
   * there is room in the socket's send buffer. */
  event_loop_add_file(plasma_state->loop, fd, EVENT_LOOP_WRITE,
                      send_notifications, plasma_state);
}

int client_index_to_socket(plasma_store_state *plasma_state,
                           int64_t client_index) {
  CHECK(client_index < utarray_len(plasma_state->clients));
  /* Return the client socket corresponding to this index. */
  return ((client *) utarray_eltptr(plasma_state->clients, client_index))->sock;
}

void process_message(event_loop *loop,
                     int client_sock,
                     void *context,
                     int events) {
  plasma_store_state *plasma_state = context;
  int64_t type;
  int64_t length;
  plasma_request *req;
  read_message(client_sock, &type, &length, (uint8_t **) &req);
  /* Get the client index corresponding to this socket. */
  client_map_entry *client_map_entry;
  HASH_FIND_INT(plasma_state->client_map, &client_sock, client_map_entry);
  CHECK(client_map_entry != NULL);
  int64_t client_index = client_map_entry->client_index;
  /* We're only sending a single object ID at a time for now. */
  plasma_reply reply;
  memset(&reply, 0, sizeof(reply));
  /* Process the different types of requests. */
  switch (type) {
  case PLASMA_CREATE:
    create_object(plasma_state, client_index, req->object_ids[0],
                  req->data_size, req->metadata_size, &reply.object);
    send_fd(client_sock, reply.object.handle.store_fd, (char *) &reply,
            sizeof(reply));
    break;
  case PLASMA_GET:
    if (get_object(plasma_state, client_index, client_sock, req->object_ids[0],
                   &reply.object) == OBJECT_FOUND) {
      send_fd(client_sock, reply.object.handle.store_fd, (char *) &reply,
              sizeof(reply));
    }
    break;
  case PLASMA_RELEASE:
    release_object(plasma_state, client_index, req->object_ids[0]);
    break;
  case PLASMA_CONTAINS:
    if (contains_object(plasma_state, req->object_ids[0]) == OBJECT_FOUND) {
      reply.has_object = 1;
    }
    plasma_send_reply(client_sock, &reply);
    break;
  case PLASMA_SEAL:
    seal_object(plasma_state, req->object_ids[0]);
    break;
  case PLASMA_DELETE:
    delete_object(plasma_state, req->object_ids[0]);
    break;
  case PLASMA_SUBSCRIBE:
    subscribe_to_updates(plasma_state, client_index, client_sock);
    break;
  case DISCONNECT_CLIENT: {
    LOG_DEBUG("Disconnecting client on fd %d", client_sock);
    /* TODO(rkn): More cleanup needs to happen here. */
    event_loop_remove_file(loop, client_sock);
  } break;
  default:
    /* This code should be unreachable. */
    CHECK(0);
  }

  free(req);
}

void new_client_connection(event_loop *loop,
                           int listener_sock,
                           void *context,
                           int events) {
  plasma_store_state *plasma_state = context;
  int new_socket = accept_client(listener_sock);
  /* Add the mapping from socket to client index to the client_index table. */
  client_map_entry *new_client_map_entry = malloc(sizeof(client_map_entry));
  new_client_map_entry->sock = new_socket;
  new_client_map_entry->client_index = utarray_len(plasma_state->clients);
  client_map_entry *old_client_map_entry;
  HASH_REPLACE_INT(plasma_state->client_map, sock, new_client_map_entry,
                   old_client_map_entry);
  if (old_client_map_entry) {
    LOG_INFO("reusing an old socket (fd %d) for a new client", new_socket);
    free(old_client_map_entry);
    /* TODO(rkn): If we reuse a socket, we have to make sure that callbacks from
     * the old client don't get used for the new client. Handle this
     * properly. */
    CHECK(0);
  }
  /* Add the new client to the array of clients. The index of the client in this
   * array will identify the client throughout this program. */
  client client = {.sock = new_socket};
  utarray_push_back(plasma_state->clients, &client);
  /* Add an event to the event loop to process messages from this client. */
  event_loop_add_file(loop, new_socket, EVENT_LOOP_READ, process_message,
                      context);
  LOG_DEBUG("new connection with fd %d", new_socket);
}

/* Report "success" to valgrind. */
void signal_handler(int signal) {
  if (signal == SIGTERM) {
    exit(0);
  }
}

void start_server(char *socket_name) {
  int socket = bind_ipc_sock(socket_name);
  CHECK(socket >= 0);
  event_loop *loop = event_loop_create();
  plasma_store_state *state = init_plasma_store(loop);
  event_loop_add_file(loop, socket, EVENT_LOOP_READ, new_client_connection,
                      state);
  event_loop_run(loop);
}

int main(int argc, char *argv[]) {
  signal(SIGTERM, signal_handler);
  char *socket_name = NULL;
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
  LOG_DEBUG("starting server listening on %s", socket_name);
  start_server(socket_name);
}

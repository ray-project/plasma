/* PLASMA MANAGER: Local to a node, connects to other managers to send and
 * receive objects from them
 *
 * The storage manager listens on its main listening port, and if a request for
 * transfering an object to another object store comes in, it ships the data
 * using a new connection to the target object manager. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <strings.h>
#include <poll.h>
#include <assert.h>
#include <netinet/in.h>
#include <netdb.h>

#include "uthash.h"
#include "utlist.h"
#include "utstring.h"
#include "common.h"
#include "io.h"
#include "event_loop.h"
#include "plasma.h"
#include "plasma_client.h"
#include "plasma_manager.h"

typedef struct {
  /** Connection to the local plasma store for reading or writing data. */
  plasma_store_conn *store_conn;
  /** Hash table of all contexts for active connections to other plasma
   * managers. These are used for writing data to other plasma stores. */
  client_connection *manager_connections;
} plasma_manager_state;

enum plasma_task_type { TASK_PULL, TASK_READ_WRITE };

typedef struct plasma_task plasma_task;

/* Buffer for reading and writing data between plasma managers. */
struct plasma_task {
  int task_type;
  object_id object_id;
  uint8_t *data;
  int64_t data_size;
  uint8_t *metadata;
  int64_t metadata_size;
  int writable;
  /* Pointer to the next buffer that we will write to this plasma manager. This
   * field is only used if we're transferring data to another plasma manager,
   * not if we are receiving data. */
  plasma_task *next;
};

/* Context for a client connection to another plasma manager. */
struct client_connection {
  /* Current state for this plasma manager. This is shared between all client
   * connections to the plasma manager. */
  plasma_manager_state *manager_state;
  /* Current position in the buffer. */
  int64_t cursor;
  /* Buffer that this connection is reading from. If this is a connection to
   * write data to another plasma store, then it is a linked list of buffers to
   * write. */
  plasma_task *transfer_queue;
  /* File descriptor for the socket connected to the other plasma manager. */
  int fd;
  /* Following fields are used only for connections to plasma managers. */
  /* Key that uniquely identifies the plasma manager that we're connected to.
   * We will use the string <address>:<port> as an identifier. */
  char *ip_addr_port;
  /** Handle for the uthash table. */
  UT_hash_handle hh;
};

plasma_manager_state *init_plasma_manager_state(const char *store_socket_name) {
  plasma_manager_state *state = malloc(sizeof(plasma_manager_state));
  state->store_conn = plasma_store_connect(store_socket_name);
  state->manager_connections = NULL;
  return state;
}

/* Handle a command request that came in through a socket (transfering data,
 * or accepting incoming data). */
void process_message(event_loop *loop,
                     int client_sock,
                     void *context,
                     int events);

void write_object_chunk(event_loop *loop,
                        int data_sock,
                        void *context,
                        int events) {
  client_connection *conn = (client_connection *) context;
  if (conn->transfer_queue == NULL) {
    /* If there are no objects to transfer, temporarily remove this connection
     * from the event loop. It will be reawoken when we receive another
     * PLASMA_TRANSFER request. */
    event_loop_remove_file(loop, conn->fd);
    return;
  }

  LOG_DEBUG("Writing data");
  ssize_t r, s;
  plasma_task *task = conn->transfer_queue;
  CHECK(task->task_type == TASK_READ_WRITE);
  if (conn->cursor == 0) {
    /* If the cursor is zero, we haven't sent any requests for this object yet,
     * so send the initial PLASMA_DATA request. */
    plasma_request manager_req = {.object_id = task->object_id,
                                  .data_size = task->data_size,
                                  .metadata_size = task->metadata_size};
    plasma_send_request(conn->fd, PLASMA_DATA, &manager_req);
  }

  /* Try to write one BUFSIZE at a time. */
  s = task->data_size + task->metadata_size - conn->cursor;
  if (s > BUFSIZE)
    s = BUFSIZE;
  write_message(conn->fd, PLASMA_DATA, s, task->data + conn->cursor);
  conn->cursor += s;
  if (s < BUFSIZE) {
    /* If we've finished writing this buffer, move on to the next transfer
     * request and reset the cursor to zero. */
    LOG_DEBUG("writing on channel %d finished", data_sock);
    conn->cursor = 0;
    LL_DELETE(conn->transfer_queue, task);
    free(task);
  }
}

void read_object_chunk(event_loop *loop,
                       int data_sock,
                       void *context,
                       int events) {
  LOG_DEBUG("Reading data");
  ssize_t r, s;
  client_connection *conn = (client_connection *) context;
  plasma_task *task = conn->transfer_queue;
  CHECK(task != NULL && task->task_type == TASK_READ_WRITE);
  /* Try to read one BUFSIZE at a time. */
  s = task->data_size + task->metadata_size - conn->cursor;
  if (s > BUFSIZE) {
    s = BUFSIZE;
  }
  int64_t type;
  read_message_into(data_sock, &type, s, task->data + conn->cursor);
  CHECK(type == PLASMA_DATA);
  conn->cursor += s;
  if (conn->cursor == task->data_size + task->metadata_size) {
    LOG_DEBUG("reading on channel %d finished", data_sock);
    plasma_seal(conn->manager_state->store_conn, task->object_id);
    LL_DELETE(conn->transfer_queue, task);
    free(task);
    /* Switch to listening for requests from this socket, instead of reading
     * data. */
    event_loop_remove_file(loop, data_sock);
    event_loop_add_file(loop, data_sock, EVENT_LOOP_READ, process_message,
                        conn);
  }
  return;
}

client_connection *get_client_connection(plasma_manager_state *state,
                                         const char *ip_addr,
                                         int port) {
  UT_string *ip_addr_port;
  utstring_new(ip_addr_port);
  utstring_printf(ip_addr_port, "%s:%d", ip_addr, port);
  client_connection *client_conn;
  HASH_FIND_STR(state->manager_connections, utstring_body(ip_addr_port),
                client_conn);
  if (!client_conn) {
    /* If we don't already have a connection to this manager, start one. */
    client_conn = malloc(sizeof(client_connection));
    client_conn->fd = plasma_manager_connect(ip_addr, port);
    client_conn->manager_state = state;
    client_conn->transfer_queue = NULL;
    client_conn->cursor = 0;
    client_conn->ip_addr_port = strdup(utstring_body(ip_addr_port));
    HASH_ADD_KEYPTR(hh, client_conn->manager_state->manager_connections,
                    client_conn->ip_addr_port,
                    strlen(client_conn->ip_addr_port), client_conn);
  }
  utstring_free(ip_addr_port);
  return client_conn;
}

void start_writing_data(event_loop *loop,
                        object_id object_id,
                        uint8_t addr[4],
                        int port,
                        client_connection *conn) {
  uint8_t *data;
  int64_t data_size;
  uint8_t *metadata;
  int64_t metadata_size;
  plasma_get(conn->manager_state->store_conn, object_id, &data_size, &data,
             &metadata_size, &metadata);
  assert(metadata == data + data_size);
  plasma_task *task = malloc(sizeof(plasma_task));
  task->task_type = TASK_READ_WRITE;
  task->object_id = object_id;
  task->data = data; /* We treat this as a pointer to the
                        concatenated data and metadata. */
  task->data_size = data_size;
  task->metadata_size = metadata_size;
  task->writable = 0;

  /* Look to see if we already have a connection to this plasma manager. */
  UT_string *ip_addr;
  utstring_new(ip_addr);
  utstring_printf(ip_addr, "%d.%d.%d.%d", addr[0], addr[1], addr[2], addr[3]);
  client_connection *client_conn =
      get_client_connection(conn->manager_state, utstring_body(ip_addr), port);
  utstring_free(ip_addr);

  if (client_conn->transfer_queue == NULL) {
    /* If we already have a connection to this manager and its inactive,
     * (re)register it with the event loop again. */
    event_loop_add_file(loop, client_conn->fd, EVENT_LOOP_WRITE,
                        write_object_chunk, client_conn);
  }
  /* Add this transfer request to this connection's transfer queue. */
  LL_APPEND(client_conn->transfer_queue, task);
}

void start_pulling_object(event_loop *loop,
                          object_id object_id,
                          uint8_t addr[4],
                          int port,
                          client_connection *conn) {
  /* TODO(pcm): remove code duplication. */
  /*
  UT_string *ip_addr;
  utstring_new(ip_addr);
  utstring_printf(ip_addr, "%d.%d.%d.%d", addr[0], addr[1], addr[2], addr[3]);
  client_connection *client_conn = get_client_connection(conn->manager_state,
  utstring_body(ip_addr), port);
  utstring_free(ip_addr);
  */
}

void start_reading_data(event_loop *loop,
                        int client_sock,
                        object_id object_id,
                        int64_t data_size,
                        int64_t metadata_size,
                        client_connection *conn) {
  plasma_task *task = malloc(sizeof(plasma_task));
  task->task_type = TASK_READ_WRITE;
  task->object_id = object_id;
  task->data_size = data_size;
  task->metadata_size = metadata_size;
  task->writable = 1;

  plasma_create(conn->manager_state->store_conn, object_id, data_size, NULL,
                metadata_size, &(task->data));
  LL_APPEND(conn->transfer_queue, task);
  conn->cursor = 0;

  /* Switch to reading the data from this socket, instead of listening for
   * other requests. */
  event_loop_remove_file(loop, client_sock);
  event_loop_add_file(loop, client_sock, EVENT_LOOP_READ, read_object_chunk,
                      conn);
}

void process_message(event_loop *loop,
                     int client_sock,
                     void *context,
                     int events) {
  client_connection *conn = (client_connection *) context;

  int64_t type;
  int64_t length;
  plasma_request *req;
  read_message(client_sock, &type, &length, (uint8_t **) &req);

  switch (type) {
  case PLASMA_PUSH:
    LOG_DEBUG("pushing object to manager with port %d", req->port);
    start_writing_data(loop, req->object_id, req->addr, req->port, conn);
    break;
  case PLASMA_PULL:
    LOG_DEBUG("pulling object from manager with port %d", req->port);
    start_pulling_object(loop, req->object_id, req->addr, req->port, conn);
  case PLASMA_DATA:
    LOG_DEBUG("starting to stream data");
    start_reading_data(loop, client_sock, req->object_id, req->data_size,
                       req->metadata_size, conn);
    break;
  case DISCONNECT_CLIENT: {
    LOG_INFO("Disconnecting client on fd %d", client_sock);
    event_loop_remove_file(loop, client_sock);
    close(client_sock);
    free(conn);
  } break;
  default:
    LOG_ERR("invalid request %" PRId64, type);
    exit(-1);
  }

  free(req);
}

void new_client_connection(event_loop *loop,
                           int listener_sock,
                           void *context,
                           int events) {
  int new_socket = accept_client(listener_sock);
  /* Create a new data connection context per client. */
  client_connection *conn = malloc(sizeof(client_connection));
  conn->manager_state = (plasma_manager_state *) context;
  conn->transfer_queue = NULL;
  event_loop_add_file(loop, new_socket, EVENT_LOOP_READ, process_message, conn);
  LOG_DEBUG("new connection with fd %d", new_socket);
}

void start_server(const char *store_socket_name,
                  const char *master_addr,
                  int port) {
  struct sockaddr_in name;
  int sock = socket(PF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    LOG_ERR("could not create socket");
    exit(-1);
  }
  name.sin_family = AF_INET;
  name.sin_port = htons(port);
  name.sin_addr.s_addr = htonl(INADDR_ANY);
  int on = 1;
  /* TODO(pcm): http://stackoverflow.com/q/1150635 */
  if (ioctl(sock, FIONBIO, (char *) &on) < 0) {
    LOG_ERR("ioctl failed");
    close(sock);
    exit(-1);
  }
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  if (bind(sock, (struct sockaddr *) &name, sizeof(name)) < 0) {
    LOG_ERR("could not bind socket");
    exit(-1);
  }
  LOG_DEBUG("listening on port %d", port);
  if (listen(sock, 5) == -1) {
    LOG_ERR("could not listen to socket");
    exit(-1);
  }

  event_loop *loop = event_loop_create();
  plasma_manager_state *state = init_plasma_manager_state(store_socket_name);
  event_loop_add_file(loop, sock, EVENT_LOOP_READ, new_client_connection,
                      state);
  event_loop_run(loop);
}

int main(int argc, char *argv[]) {
  /* Socket name of the plasma store this manager is connected to. */
  char *store_socket_name = NULL;
  /* IP address of this node. */
  char *master_addr = NULL;
  /* Port number the manager should use. */
  int port;
  int c;
  while ((c = getopt(argc, argv, "s:m:p:")) != -1) {
    switch (c) {
    case 's':
      store_socket_name = optarg;
      break;
    case 'm':
      master_addr = optarg;
      break;
    case 'p':
      port = atoi(optarg);
      break;
    default:
      LOG_ERR("unknown option %c", c);
      exit(-1);
    }
  }
  if (!store_socket_name) {
    LOG_ERR(
        "please specify socket for connecting to the plasma store with -s "
        "switch");
    exit(-1);
  }
  if (!master_addr) {
    LOG_ERR(
        "please specify ip address of the current host in the format "
        "123.456.789.10 with -m switch");
    exit(-1);
  }
  start_server(store_socket_name, master_addr, port);
}

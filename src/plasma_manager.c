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

#include "common.h"
#include "io.h"
#include "event_loop.h"
#include "plasma.h"
#include "plasma_client.h"
#include "plasma_manager.h"

/* Buffer for reading and writing data between plasma managers. */
typedef struct {
  object_id object_id;
  uint8_t *data;
  int64_t data_size;
  uint8_t *metadata;
  int64_t metadata_size;
  int writable;
} plasma_buffer;

/* Context for a client connection to another plasma manager. */
typedef struct {
  /* Connection to the local plasma store for reading or writing data. This is
   * shared between all clients. */
  plasma_store_conn *store_conn;
  /* Buffer this connection is reading from or writing to. */
  plasma_buffer buf;
  /* Current position in the buffer. */
  int64_t cursor;
} data_connection;

void write_data(event_loop *loop, int data_sock, void *context, int events) {
  LOG_DEBUG("Writing data");
  ssize_t r, s;
  data_connection *conn = (data_connection *) context;
  s = conn->buf.data_size + conn->buf.metadata_size - conn->cursor;
  if (s > BUFSIZE)
    s = BUFSIZE;
  r = write(data_sock, conn->buf.data + conn->cursor, s);
  if (r != s) {
    if (r > 0) {
      LOG_ERR("partial write on fd %d", data_sock);
    } else {
      LOG_ERR("write error");
      exit(-1);
    }
  } else {
    conn->cursor += r;
  }
  if (r == 0) {
    LOG_DEBUG("writing on channel %d finished", data_sock);
    event_loop_remove_file(loop, data_sock);
    free(conn);
    close(data_sock);
  }
}

void read_data(event_loop *loop, int data_sock, void *context, int events) {
  LOG_DEBUG("Reading data");
  ssize_t r;
  data_connection *conn = (data_connection *) context;
  r = read(data_sock, conn->buf.data + conn->cursor, BUFSIZE / 2);
  if (r == -1) {
    LOG_ERR("read error");
  } else if (r == 0) {
    LOG_INFO("end of file");
  } else {
    conn->cursor += r;
  }
  if (r == 0) {
    LOG_DEBUG("reading on channel %d finished", data_sock);
    plasma_seal(conn->store_conn, conn->buf.object_id);
    event_loop_remove_file(loop, data_sock);
    free(conn);
    close(data_sock);
  }
  return;
}

/* Start transfering data to another object store manager. This establishes
 * a connection to the remote manager and sends the data header to the other
 * object manager. */
void initiate_transfer(event_loop *loop,
                       plasma_request *req,
                       data_connection *conn) {
  uint8_t *data;
  int64_t data_size;
  uint8_t *metadata;
  int64_t metadata_size;
  plasma_get(conn->store_conn, req->object_id, &data_size, &data,
             &metadata_size, &metadata);
  assert(metadata == data + data_size);
  plasma_buffer buf = {.object_id = req->object_id,
                       .data = data, /* We treat this as a pointer to the
                                        concatenated data and metadata. */
                       .data_size = data_size,
                       .metadata_size = metadata_size,
                       .writable = 0};
  char ip_addr[32];
  snprintf(ip_addr, 32, "%d.%d.%d.%d", req->addr[0], req->addr[1], req->addr[2],
           req->addr[3]);

  int fd = plasma_manager_connect(&ip_addr[0], req->port);
  data_connection *transfer_conn = malloc(sizeof(data_connection));
  transfer_conn->store_conn = conn->store_conn;
  transfer_conn->buf = buf;
  transfer_conn->cursor = 0;
  event_loop_add_file(loop, fd, EVENT_LOOP_WRITE, write_data, transfer_conn);
  plasma_request manager_req = {.object_id = req->object_id,
                                .data_size = buf.data_size,
                                .metadata_size = buf.metadata_size};
  plasma_send_request(fd, PLASMA_DATA, &manager_req);
}

/* Start reading data from another object manager.
 * Initializes the object we are going to write to in the
 * local plasma store and then switches the data socket to reading mode. */
void start_reading_data(event_loop *loop,
                        int client_sock,
                        plasma_request *req,
                        data_connection *conn) {
  plasma_buffer buf = {.object_id = req->object_id,
                       .data_size = req->data_size,
                       .metadata_size = req->metadata_size,
                       .writable = 1};
  plasma_create(conn->store_conn, req->object_id, req->data_size, NULL,
                req->metadata_size, &buf.data);
  conn->buf = buf;
  conn->cursor = 0;

  event_loop_remove_file(loop, client_sock);
  event_loop_add_file(loop, client_sock, EVENT_LOOP_READ, read_data, conn);
}

/* Handle a command request that came in through a socket (transfering data,
 * or accepting incoming data). */
void process_message(event_loop *loop,
                     int client_sock,
                     void *context,
                     int events) {
  data_connection *conn = (data_connection *) context;

  int64_t type;
  int64_t length;
  plasma_request *req;
  read_message(client_sock, &type, &length, (uint8_t **) &req);

  switch (type) {
  case PLASMA_TRANSFER:
    LOG_INFO("transfering object to manager with port %d", req->port);
    initiate_transfer(loop, req, conn);
    break;
  case PLASMA_DATA:
    LOG_INFO("starting to stream data");
    start_reading_data(loop, client_sock, req, conn);
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
}

void new_client_connection(event_loop *loop,
                           int listener_sock,
                           void *context,
                           int events) {
  int new_socket = accept_client(listener_sock);
  /* Create a new data connection context per client. */
  data_connection *conn = malloc(sizeof(data_connection));
  conn->store_conn = (plasma_store_conn *) context;
  event_loop_add_file(loop, new_socket, EVENT_LOOP_READ, process_message, conn);
  LOG_INFO("new connection with fd %d", new_socket);
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
  if (ioctl(sock, FIONBIO, (char*) &on) < 0) {
    LOG_ERR("ioctl failed");
    close(sock);
    exit(-1);
  }
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  if (bind(sock, (struct sockaddr*) &name, sizeof(name)) < 0) {
    LOG_ERR("could not bind socket");
    exit(-1);
  }
  LOG_INFO("listening on port %d", port);
  if (listen(sock, 5) == -1) {
    LOG_ERR("could not listen to socket");
    exit(-1);
  }

  event_loop *loop = event_loop_create();
  plasma_store_conn *conn = plasma_store_connect(store_socket_name);
  event_loop_add_file(loop, sock, EVENT_LOOP_READ, new_client_connection,
                      conn);
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

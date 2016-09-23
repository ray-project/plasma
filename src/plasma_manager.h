#ifndef PLASMA_MANAGER_H
#define PLASMA_MANAGER_H

#include <poll.h>
#include "utarray.h"

/* Start writing the object with id "object_id" to the plasma manager with
 * with connection index "conn_index". */
void start_write_object(plasma_manager_state *s, object_id object_id, int64_t conn_index);

/* Start reading the object with id "object_id" from the plasma manager with
 * connection index "conn_index". */
void start_read_object(plasma_manager_state *s, object_id object_id, int64_t conn_index);

/* Read the next chunk of the object with id "object_id" from the plasma manager
 * that is connected to the connection with index "conn_index". */
void read_object_chunk(plasma_manager_state *s, object_id object_id, int64_t conn_index);

/* Write the next chunk of the object with id "object_id" to the plasma manager
 * that is connected to the connection with index "conn_index". */
void write_object_chunk(plasma_manager_state *s, object_id object_id, int64_t conn_index);

/* Fetch object with id object_id from plasma manager at address addr. */
void fetch_object(plasma_manager_state *s, object_id object_id, manager_addr addr);

/* The buffer size in bytes. Data will get transfered in multiples of this */
#define BUFSIZE 4096

enum connection_type { CONNECTION_REDIS, CONNECTION_LISTENER, CONNECTION_DATA };

enum data_connection_type {
  /* Connection to send commands and metadata to the manager. */
  DATA_CONNECTION_HEADER,
  /* Connection to send data to another manager. */
  DATA_CONNECTION_WRITE,
  /* Connection to receive data from another manager. */
  DATA_CONNECTION_READ
};

typedef struct {
  /* Of type data_connection_type. */
  int type;
  /* Local socket of the plasma store that is accessed for reading or writing
   * data for this connection. */
  int store_conn;
  /* Buffer this connection is reading from or writing to. */
  plasma_buffer buf;
  /* Current position in the buffer. */
  int64_t cursor;
} data_connection;

#endif

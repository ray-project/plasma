#ifndef PLASMA_MANAGER_H
#define PLASMA_MANAGER_H

#include <poll.h>
#include "utarray.h"

/* The buffer size in bytes. Data will get transfered in multiples of this */
#define BUFSIZE 4096

typedef struct {
  /* Connection to the local plasma store for reading or writing data. */
  plasma_store_conn *store_conn;
  /* Buffer this connection is reading from or writing to. */
  plasma_buffer buf;
  /* Current position in the buffer. */
  int64_t cursor;
} data_connection;

#endif

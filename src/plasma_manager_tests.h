/**
 * This file contains all definitions that are internal to the plasma manager
 * code but that are needed by the unit tests in test/manager_tests.c. This
 * includes structs instantiated by the unit tests and forward declarations for
 * functions used internally by the plasma manager code.
 */

/* Buffer for requests between plasma managers. */
typedef struct plasma_request_buffer plasma_request_buffer;
struct plasma_request_buffer {
  int type;
  object_id object_id;
  uint8_t *data;
  int64_t data_size;
  uint8_t *metadata;
  int64_t metadata_size;
  /* Pointer to the next buffer that we will write to this plasma manager. This
   * field is only used if we're pushing requests to another plasma manager,
   * not if we are receiving data. */
  plasma_request_buffer *next;
};

/**
 * Create a new context for the given object ID with the given
 * client connection and register it with the manager's
 * outstanding fetch or wait requests and the client
 * connection's active object contexts.
 *
 * @param client_conn The client connection context.
 * @param object_id The object ID whose context we want to
 *        create.
 * @return A pointer to the newly created object context.
 */
client_object_connection *add_object_connection(client_connection *client_conn,
                                                object_id object_id);

/**
 * Given an object ID and the managers it can be found on, start requesting a
 * transfer from the managers.
 *
 * @param object_id The object ID we want to request a transfer of.
 * @param manager_count The number of managers the object can be found on.
 * @param manager_vector A vector of the IP addresses of the managers that the
 *        object can be found on.
 * @param context The context for the connection to this client.
 *
 * Initializes a new context for this client and object. Managers are tried in
 * order until we receive the data or we timeout and run out of retries.
 */
void request_transfer(object_id object_id,
                      int manager_count,
                      const char *manager_vector[],
                      void *context);

/**
 * Clean up and free an active object context. Deregister it from the
 * associated client connection and from the manager state.
 *
 * @param client_conn The client connection context.
 * @param object_id The object ID whose context we want to delete.
 */
void remove_object_connection(client_connection *client_conn,
                              client_object_connection *object_conn);

/**
 * Get a connection to the remote manager at the specified address. Creates a
 * new connection to this manager if one doesn't already exist.
 *
 * @param state Our plasma manager state.
 * @param ip_addr The IP address of the remote manager we want to connect to.
 * @param port The port that the remote manager is listening on.
 * @return A pointer to the connection to the remote manager.
 */
client_connection *get_manager_connection(plasma_manager_state *state,
                                          const char *ip_addr,
                                          int port);

/**
 * Reads an object chunk sent by the given client into a buffer. This is the
 * complement to write_object_chunk.
 *
 * @param conn The connection to the client who's sending the data.
 * @param buf The buffer to write the data into.
 * @return An integer representing whether the client is done
 *         sending this object. 1 means that the client has
 *         sent all the data, 0 means there is more.
 */
int read_object_chunk(client_connection *conn, plasma_request_buffer *buf);
/**
 * Writes an object chunk from a buffer to the given client. This is the
 * complement to read_object_chunk.
 *
 * @param conn The connection to the client who's receiving the data.
 * @param buf The buffer to read data from.
 * @return Void.
 */
void write_object_chunk(client_connection *conn, plasma_request_buffer *buf);

/**
 * Get the event loop of the given plasma manager state.
 *
 * @param state The state of the plasma manager whose loop we want.
 * @return A pointer to the manager's event loop.
 */
event_loop *get_event_loop(plasma_manager_state *state);
/**
 * Get the file descriptor for the given client's socket. This is the socket
 * that the client sends or reads data through.
 *
 * @param conn The connection to the client who's sending or reading data.
 * @return A file descriptor for the socket.
 */
int get_client_sock(client_connection *conn);

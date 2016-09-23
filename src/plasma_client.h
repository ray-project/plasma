#ifndef PLASMA_CLIENT_H
#define PLASMA_CLIENT_H

/* Connect to the local plasma store UNIX domain socket with path socket_name
 * and return the resulting connection. */
plasma_store_conn *plasma_store_connect(const char *socket_name);

/* Connect to a possibly remote plasma manager */
int plasma_manager_connect(const char *addr, int port);

/* Create a new object with an object_id provided by the caller, a given
 * object size in bytes and given metadata. A pointer to the newly created
 * object will be stored in the "data" out parameter. After this call returns,
 * the client process can write to the object. After it is done writing,
 * the client should call "seal" to make the object immutable so it can
 * be shared with other processes. */
void plasma_create_object(plasma_store_conn *conn,
                          plasma_id object_id,
                          int64_t size,
                          uint8_t *metadata,
                          int64_t metadata_size,
                          uint8_t **data);

/* Resize an obect that has been created by the same client and that has
 * not been sealed yet. All existing pointers to the object will get
 * invalidated. */
void plasma_resize_object(plasma_store_conn *conn,
                          plasma_id object_id,
                          int64_t new_size,
                          uint8_t **data);

/* Get an object. If the object has not been sealed yet, this call will block
 * until the object is sealed. All or size, data, metadata_size and metadata
 * are out parameters that will be set by this function. */
void plasma_get_object(plasma_store_conn *conn,
                       plasma_id object_id,
                       int64_t *size,
                       uint8_t **data,
                       int64_t *metadata_size,
                       uint8_t **metadata);

/* Seal an object. This function makes the object immutable and unblocks
 * all processes waiting for the object in a "get_object" call. */
void plasma_seal_object(plasma_store_conn *conn, plasma_id object_id);

/* Delete the object from this plasma store. */
void plasma_delete_object(plasma_store_conn *conn, plasma_id object_id);

/* Mark this object as not used any more by this client. Before a deleted
 * object can actually be freed, all clients using the object need to
 * unmap it. */
void plasma_unmap_object(plasma_id object_id);

/* Fetch an object from a remote plasma store that has the object stored. */
void plasma_fetch_object(plasma_store_conn *conn, plasma_id object_id);


#endif

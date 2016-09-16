#ifndef PLASMA_STORE_H
#define PLASMA_STORE_H

/* Create a new object:
 *
 * req->object_id: Object ID of the object to be created.
 * req->data_size: Size in bytes of the object to be created.
 * req->metadata_size: Size in bytes of the object metadata.
 *
 * TODO(pcm): Define return values.
 */
void create_object(int conn, plasma_request* req);

/* Get an object:
 *
 * req->object_id: Object ID of the object to be gotten.
 *
 * If the object is available, it should be returned to conn
 * via send_fd. Else, conn should be notified when the object
 * is available.
 */
void get_object(int conn, plasma_request* req);

/* Seal an object:
 *
 * req->object_id: Object ID of the object to be sealed.
 *
 * Should notify all the sockets waiting for the object.
 */
void seal_object(int conn, plasma_request* req);

#endif

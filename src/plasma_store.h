#ifndef PLASMA_STORE_H
#define PLASMA_STORE_H

/* Handle to access memory mapped file and map it into client address space */
struct {
  int fd;
  int64_t mmap_size;
} mmap_handle;

struct {
  /* Handle for memory mapped file the object is stored in. */
  mmap_handle handle;
  /* The offset in the memory mapped file of the data. */
  ptrdiff_t data_offset;
  /* The offset in the memory mapped file of the metadata. */
  ptrdiff_t metadata_offset;
  /* The size of the data. */
  int64_t data_size;
  /* The size of the metadata. */
  int64_t metadata_size;
} plasma_object;

/* Create a new object:
 *
 * object_id: Object ID of the object to be created.
 * data_size: Size in bytes of the object to be created.
 * metadata_size: Size in bytes of the object metadata.
 */
plasma_object create_object(int conn, plasma_id object_id, int64_t data_size, int64_t metadata_size);

/* Get an object:
 *
 * object_id: Object ID of the object to be gotten.
 *
 * If the object is available, it should be returned to conn
 * via send_fd. Else, conn should be notified when the object
 * is available.
 */
plasma_object get_object(int conn, plasma_id object_id);

/* Seal an object:
 *
 * req->object_id: Object ID of the object to be sealed.
 *
 * Should notify all the sockets waiting for the object.
 */
void seal_object(int conn, plasma_id object_id);

#endif

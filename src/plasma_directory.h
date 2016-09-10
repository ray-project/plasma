/* DATABASE: This file contains the API that lets the object store talk to
 * the object table. */

#ifndef PLASMA_DIRECTORY_H
#define PLASMA_DIRECTORY_H

#include <hiredis/hiredis.h>
#include <hiredis/async.h>

#include "plasma.h"
#include "event_loop.h"

typedef struct {
  /* Manager ID that this plasma directory is part of (assigned by redis). */
  int manager_id;
  /* Redis context for this directory. */
  redisAsyncContext* context;
  /* Which events are we processing (read, write)? */
  int reading, writing; 
  /* The plasma manager event loop. */
  event_loop* loop;
} plasma_directory;

/* Waits until the object becomes available and then returns. */
// void plasma_directory_lookup(plasma_manager_state* state, plasma_id object_id, int conn_idx);

/* Link a new object in the directory. */
void plasma_directory_link(plasma_directory *directory, plasma_id object_id);

/* Initialize the plasma directory and connect it to redis. */
void plasma_directory_init(plasma_directory *directory, const char* manager_address, int manager_port, const char *directory_address, int directory_port);

// void plasma_directory_disconnect(plasma_directory *directory);

/* Attach plasma directory to plasma manager run loop. */
int plasma_directory_attach(plasma_directory *directory, event_loop *loop);

/* Should be called by the event loop whenever a new event is triggered on the file descriptor */
void plasma_directory_event(plasma_directory *directory);



#endif

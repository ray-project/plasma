#ifndef MALLOC_H
#define MALLOC_H

#define DEFAULT_GRANULARITY ((size_t) 128U * 1024U)

void get_malloc_mapinfo(void *addr,
                        int *fd,
                        int64_t *map_length,
                        ptrdiff_t *offset);

#endif /* MALLOC_H */

#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <fcntl.h>
#include <libavutil/frame.h>

#define SHARED_MEM_MAX_SIZE 16777216

typedef struct {
	uint32_t size;
	int fd;
  char backing_file[20];
	caddr_t memory;
	sem_t *semaphore;
	int step;
} SharedMemory;

void dispose_shared_memory(SharedMemory *shared);
SharedMemory *create_shared_memory(char *file, uint32_t size);
int write_shared_memory(SharedMemory *shared, void *data, uint32_t size);

#endif

#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <fcntl.h>
#include <libavutil/frame.h>

#include "mlt_log.h"
#include "shared_memory.h"

void dispose_shared_memory(SharedMemory *shared) {
	if (shared->memory)
		munmap(shared->memory, shared->size); /* unmap the storage */
	if (shared->fd)
  	close(shared->fd);
	if (shared->semaphore)
  	sem_close(shared->semaphore);
	if (shared->backing_file)
  	shm_unlink(shared->backing_file); /* unlink from the backing file */
	if (shared)
		free(shared);
}

SharedMemory *create_shared_memory(char *file, uint32_t size) {
	if (size > SHARED_MEM_MAX_SIZE) {
		mlt_log_error(NULL, "tried to allocate too much shared memory. %zu > %zu\n", size, SHARED_MEM_MAX_SIZE);
		exit(-1);
	}

  SharedMemory *shared = (SharedMemory *)malloc(sizeof(SharedMemory));

	strcpy(shared->backing_file, file);
	shared->memory = NULL;
	shared->semaphore = NULL;
	shared->size = size;

	int fd = shm_open(shared->backing_file, O_RDWR | O_CREAT, 0644);
	if (fd < 0) {
		return NULL;
	}
	
	shared->fd = fd;
	ftruncate(fd, size);

	shared->memory = (caddr_t)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (shared->memory == (caddr_t)-1) {
		shared->memory = NULL;
		dispose_shared_memory(shared);
		return NULL;
	}

	shared->semaphore = sem_open(shared->backing_file, O_CREAT, 0644, 0);
	if (shared->semaphore == (void*)-1) {
		shared->semaphore = NULL;
		dispose_shared_memory(shared);
		return NULL;
	}

  return shared;
}

int write_shared_memory(SharedMemory *shared, void *data, uint32_t size) {
	if (size > shared->size) {
		mlt_log_warning(NULL, "data provided larger than allocated shared memory (%zu > %zu). writing truncated data\n", size, shared->size);
		size = shared->size;
	}

	int locked;
	if (shared->semaphore && !sem_getvalue(shared->semaphore, &locked)) { // && !locked) {
		memcpy(shared->memory, data, size);
		return 0;
		// return sem_post(shared->semaphore) < 0;
	}
	return -1;
}

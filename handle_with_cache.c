#define _POSIX_SOURCE
#include <stdlib.h>
#include <fcntl.h>
#include <curl/curl.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <mqueue.h>
#include <sys/mman.h>
#include <semaphore.h>

#include <pthread.h>
#include "gfserver.h"

#define QUEUE_NAME            "/simplecache_mq"

static steque_t *seg_q;
static pthread_mutex_t *seg_q_mutex;
static pthread_cond_t *seg_q_cond;
static unsigned long seg_size;

struct shm_info {
  int  memfd;
  char mem_name[12];
  char sem1_name[12];
  char sem2_name[12];
};

struct mem_info {
  char mem_name[12];
  char sem1_name[12];
  char sem2_name[12];
};

struct request_info {
	struct mem_info mem_i;
	int mem_size;
	int file_len;
	char file_path[0];
};

int handle_with_cache_init(steque_t *segfds_q, unsigned long segment_size,
		pthread_mutex_t *segfds_q_mutex, pthread_cond_t *segfds_q_cond)
{
	seg_q = segfds_q;
	seg_size = segment_size;
	seg_q_mutex = segfds_q_mutex;
	seg_q_cond = segfds_q_cond;

	return 0;
}

ssize_t handle_with_cache(gfcontext_t *ctx, char *path, void* arg)
{
	mqd_t msg_q;
	void *mem;
	struct request_info *req;
	struct shm_info *shm_blk;
	sem_t *sem1;
	sem_t *sem2;
	int file_in_cache;
	size_t file_size = 0;
	size_t bytes_transferred = 0;
	ssize_t write_len;
	size_t cache_file_size;

	pthread_mutex_lock(seg_q_mutex);
	while (steque_isempty(seg_q)) {
		pthread_cond_wait(seg_q_cond, seg_q_mutex);
	}
	shm_blk = (struct shm_info *)steque_pop(seg_q);
	pthread_mutex_unlock(seg_q_mutex);

	if ((sem1 = sem_open(shm_blk->sem1_name, O_CREAT, 0644, 0)) ==
	    SEM_FAILED) {
		perror("sem_open");
		return -1;
    	}
    	if ((sem2 = sem_open(shm_blk->sem2_name, O_CREAT, 0644, 0)) ==
	    SEM_FAILED) {
		perror("sem_open");
		return -1;
    	}
retry:
	errno = 0;
	msg_q = mq_open(QUEUE_NAME, O_WRONLY);
	if (msg_q == -1) {
		if (errno == ENOENT || errno == EACCES) {
			/* simplecached isn't ready yet, sleep and then retry */
			fprintf(stdout, "waiting for simplecached\n");
			sleep(2);
			goto retry;
		}
		perror("mq_open");
		return -1;
	}

	mem = mmap(NULL, seg_size, PROT_READ | PROT_WRITE, MAP_SHARED,
	    shm_blk->memfd, 0);
	if (!mem) {
		perror("mmap");
		return -1;
	}
	req = malloc(sizeof(*req) + strlen(path) + 1);
	memcpy(&req->mem_i, (char *)shm_blk + sizeof(int), sizeof(req->mem_i));
	req->mem_size = seg_size;
	req->file_len = strlen(path) + 1;
	strncpy(req->file_path, path, strlen(path));
	req->file_path[strlen(path)] = '\0';
	mq_send(msg_q, (char *)req, sizeof(*req) + strlen(path) + 1, 0);
	free(req);

	sem_wait(sem1);
	file_in_cache = *(int *)mem;
	sem_post(sem1);

	if (file_in_cache == -1) {
		 gfs_sendheader(ctx, GF_FILE_NOT_FOUND, 0);
		 goto finish;
	}
	sem_wait(sem1);
	file_size = *(size_t *)mem;
	cache_file_size = file_size;
	gfs_sendheader(ctx, GF_OK, file_size);
	sem_post(sem1);
	if (!file_size) {
		goto finish;
	}
	while (file_size) {
		sem_wait(sem1);
		bytes_transferred =  seg_size < file_size ?
		    seg_size : file_size;
		write_len = gfs_send(ctx, (char *)mem, bytes_transferred);
		if (write_len != bytes_transferred) {
			fprintf(stderr, "write error");
		}
		file_size -= bytes_transferred;
		sem_post(sem1);
	}
	sem_wait(sem1);
	file_size = *(size_t *)mem;
	if (file_size) {
		fprintf(stderr, "transfer error");
	}
	sem_post(sem1);

finish:
	mq_close(msg_q);
	sem_close(sem1);
	sem_close(sem2);
	sem_unlink(shm_blk->sem1_name);
	sem_unlink(shm_blk->sem2_name);
	munmap(mem, seg_size);
	pthread_mutex_lock(seg_q_mutex);
	steque_push(seg_q, shm_blk);
	pthread_mutex_unlock(seg_q_mutex);
	pthread_cond_signal(seg_q_cond);

	return cache_file_size;
}


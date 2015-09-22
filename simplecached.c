#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <pthread.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <mqueue.h>
#include <sys/mman.h>
#include <semaphore.h>

#include "shm_channel.h"
#include "simplecache.h"
#include "steque.h"

#define MAX_CACHE_REQUEST_LEN 512
#define MAX_THREADS	      1000
#define QUEUE_NAME            "/simplecache_mq"

static steque_t reqs_q;
static pthread_mutex_t reqs_q_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reqs_q_cond = PTHREAD_COND_INITIALIZER;
static mqd_t msg_q;

struct mem_info {
  char mem_name[12];
  char sem1_name[12];
  char sem2_name[12];
};

struct request_info {
	struct mem_info mem_i;
	int mem_size;
	int file_len;
	char *file_path;
};

static void _sig_handler(int signo){
	if (signo == SIGINT || signo == SIGTERM){
		pthread_cond_destroy(&reqs_q_cond);
		pthread_mutex_destroy(&reqs_q_mutex);
		mq_close(msg_q);
		if (mq_unlink(QUEUE_NAME) == 0) {
			fprintf(stdout, "Message queue %s removed from system.\n",
			    QUEUE_NAME);
		}
		exit(signo);
	}
}

#define USAGE                                                                 \
"usage:\n"                                                                    \
"  simplecached [options]\n"                                                  \
"options:\n"                                                                  \
"  -t [thread_count]   Num worker threads (Default: 1, Range: 1-1000)\n"      \
"  -c [cachedir]       Path to static files (Default: ./)\n"                  \
"  -h                  Show this help message\n"                              

/* OPTIONS DESCRIPTOR ====================================================== */
static struct option gLongOptions[] = {
  {"nthreads",           required_argument,      NULL,           't'},
  {"cachedir",           required_argument,      NULL,           'c'},
  {"help",               no_argument,            NULL,           'h'},
  {NULL,                 0,                      NULL,             0}
};

void Usage() {
  fprintf(stdout, "%s", USAGE);
}

static void *simplecached_worker(void *arg)
{
	struct request_info *req;
	void *mem;
	int cache_fd;
	sem_t *sem1;
	sem_t *sem2;
	ssize_t read_len;
	size_t file_len, bytes_transferred;
	int mem_fd;

	while (1) {
		pthread_mutex_lock(&reqs_q_mutex);
		while (steque_isempty(&reqs_q)) {
			pthread_cond_wait(&reqs_q_cond, &reqs_q_mutex);
		}
		req = (struct request_info *)steque_pop(&reqs_q);
		pthread_mutex_unlock(&reqs_q_mutex);
		mem_fd = shm_open(req->mem_i.mem_name, O_RDWR, 0777);
		if (mem_fd == -1) {
			perror("shm_open");
			goto finish;
		}
		if ((sem1 = sem_open(req->mem_i.sem1_name, O_CREAT, 0644, 0)) ==
		    SEM_FAILED) {
			perror("sem_open");
			goto finish;
    		}
    		if ((sem2 = sem_open(req->mem_i.sem2_name, O_CREAT, 0644, 0)) ==
		    SEM_FAILED) {
			perror("sem_open");
			goto finish;
    		}

		mem = mmap(NULL, req->mem_size, PROT_READ | PROT_WRITE,
		    MAP_SHARED, mem_fd, 0);
		if (!mem) {
			perror("mmap");
			goto finish;
		}
		cache_fd = simplecache_get(req->file_path);
		*(int *)mem = cache_fd == -1 ? -1 : 1;
		sem_post(sem1);
		sem_wait(sem1);
		if (cache_fd == -1) {
			goto finish;
		}
		file_len = lseek(cache_fd, 0, SEEK_END);
		lseek(cache_fd, 0, SEEK_SET);
		*(size_t *)mem = file_len;

		sem_post(sem1);
		sem_wait(sem1);

		if (!file_len) {
			goto finish;
		}
		/* Sending the file contents chunk by chunk. */
		bytes_transferred = 0;
		while (bytes_transferred < file_len) {
			read_len = read(cache_fd, mem, req->mem_size);
			if (read_len < 0){
				perror("read");
			}
			bytes_transferred += read_len;
			sem_post(sem1);
			sem_wait(sem1);
		}
		*(size_t *)mem = 0;
		sem_post(sem1);
		sem_wait(sem1);
finish:
		sem_close(sem1);
		sem_close(sem2);
		munmap(mem, req->mem_size);
		free(req->file_path);
		free(req);
	}

	return NULL;
}

int main(int argc, char **argv) {
	pthread_t thread[MAX_THREADS];
	int nthreads = 1;
	int i;
	char *cachedir = "locals.txt";
	char option_char;
	struct mq_attr msg_q_attr;
	ssize_t num_bytes_recvd;
	char *request_str;
	struct request_info *req;
	char *ptr;

	while ((option_char = getopt_long(argc, argv, "t:c:h", gLongOptions, NULL)) != -1) {
		switch (option_char) {
			case 't': // thread-count
				nthreads = atoi(optarg);
				break;   
			case 'c': //cache directory
				cachedir = optarg;
				break;
			case 'h': // help
				Usage();
				exit(0);
				break;    
			default:
				Usage();
				exit(1);
		}
	}

	if (signal(SIGINT, _sig_handler) == SIG_ERR){
		fprintf(stderr,"Can't catch SIGINT...exiting.\n");
		exit(EXIT_FAILURE);
	}

	if (signal(SIGTERM, _sig_handler) == SIG_ERR){
		fprintf(stderr,"Can't catch SIGTERM...exiting.\n");
		exit(EXIT_FAILURE);
	}

	/* Initializing the cache */
	simplecache_init(cachedir);

	steque_init(&reqs_q);

	msg_q_attr.mq_flags = 0;
	msg_q_attr.mq_maxmsg = 10;
	msg_q_attr.mq_msgsize = MAX_CACHE_REQUEST_LEN;
	msg_q_attr.mq_curmsgs = 0;

	if (mq_unlink(QUEUE_NAME) == 0) {
		fprintf(stdout, "Message queue %s removed from system.\n",
		    QUEUE_NAME);
	}
	msg_q = mq_open(QUEUE_NAME, O_CREAT | O_RDONLY,
	    0777, &msg_q_attr);
	if (msg_q == -1) {
		perror("mq_open");
		exit(1);
	}

	/*
	 * Start the worker threads
	 */
	for (i = 0; i < nthreads; i++) {
		pthread_create(&thread[i], NULL, simplecached_worker, NULL);
	}
	for (;;) {
		request_str = malloc(MAX_CACHE_REQUEST_LEN + 1);
		num_bytes_recvd = mq_receive(msg_q, request_str,
		    MAX_CACHE_REQUEST_LEN + 1, NULL);
		if (num_bytes_recvd < 0) {
			perror("mq_receive");
			break;
		}
		req = malloc(sizeof(*req));
		ptr = request_str;
		strncpy(req->mem_i.mem_name, ptr,
		    sizeof(req->mem_i.mem_name));
		ptr += sizeof(req->mem_i.mem_name);
		strncpy(req->mem_i.sem1_name, ptr,
		    sizeof(req->mem_i.sem1_name));
		ptr += sizeof(req->mem_i.sem1_name);
		strncpy(req->mem_i.sem2_name, ptr,
		    sizeof(req->mem_i.sem2_name));
		ptr += sizeof(req->mem_i.sem2_name);
		req->mem_size = *(int *)(ptr);
		ptr += sizeof(int);
		req->file_len = *(int *)(ptr);
		ptr += sizeof(int);
		req->file_path = malloc(req->file_len);
		strncpy(req->file_path, ptr, req->file_len);
		free(request_str);
		pthread_mutex_lock(&reqs_q_mutex);
		steque_push(&reqs_q, req);
		pthread_mutex_unlock(&reqs_q_mutex);
		pthread_cond_signal(&reqs_q_cond);
	}

}
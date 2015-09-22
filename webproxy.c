#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <limits.h>
#include <unistd.h>
#include <semaphore.h>

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>

#include "steque.h"
#include "gfserver.h"
                                                                \
#define USAGE                                                                 \
"usage:\n"                                                                    \
"  webproxy [options]\n"                                                     \
"options:\n"                                                                  \
"  -n [num_segments]   number of segments to use in communication with cache. (Default: 1)\n" \
"  -z [segment_size]   the size (in bytes) of the segments. (Default: 1024) \n"               \
"  -p [listen_port]    Listen port (Default: 8888)\n"                         \
"  -t [thread_count]   Num worker threads (Default: 1, Range: 1-1000)\n"      \
"  -s [server]         The server to connect to (Default: Udacity S3 instance)"\
"  -h                  Show this help message\n"                              \
"special options:\n"                                                          \
"  -d [drop_factor]    Drop connects if f*t pending requests (Default: 5).\n"


/* OPTIONS DESCRIPTOR ====================================================== */
static struct option gLongOptions[] = {
  {"num_segments",  required_argument,      NULL,           'n'},
  {"segment_size",  required_argument,      NULL,           'z'},
  {"port",          required_argument,      NULL,           'p'},
  {"thread-count",  required_argument,      NULL,           't'},
  {"server",        required_argument,      NULL,           's'},         
  {"help",          no_argument,            NULL,           'h'},
  {NULL,            0,                      NULL,             0}
};

extern ssize_t handle_with_cache(gfcontext_t *ctx, char *path, void* arg);
int handle_with_cache_init(steque_t *segfds_q, unsigned long segment_size,
    pthread_mutex_t *segfds_q_mutex, pthread_cond_t *segfds_q_cond);

static gfserver_t gfs;
static steque_t segfds_q;
static pthread_mutex_t segfds_q_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t segfds_q_cond = PTHREAD_COND_INITIALIZER;

struct shm_info {
  int  memfd;
  char mem_name[12];
  char sem1_name[12];
  char sem2_name[12];
};

static void _sig_handler(int signo){
  struct shm_info *shm_blk;

  if (signo == SIGINT || signo == SIGTERM){
    gfserver_stop(&gfs);
    pthread_mutex_lock(&segfds_q_mutex);
    while (!steque_isempty(&segfds_q)) {
        shm_blk = (struct shm_info *)steque_pop(&segfds_q);
        if (shm_unlink(shm_blk->mem_name) == 0) {
          fprintf(stdout, "Shared mem %s removed from system.\n",
            shm_blk->mem_name);
        }
        if (sem_unlink(shm_blk->sem1_name) == 0) {
          fprintf(stdout, "Semaphore %s removed from system.\n",
            shm_blk->sem1_name);
        }
        if (sem_unlink(shm_blk->sem2_name) == 0) {
          fprintf(stdout, "Semaphore %s removed from system.\n",
            shm_blk->sem1_name);
        }
    }
    pthread_mutex_unlock(&segfds_q_mutex);
    pthread_cond_destroy(&segfds_q_cond);
    pthread_mutex_destroy(&segfds_q_mutex);
    exit(signo);
  }
}


/* Main ========================================================= */
int main(int argc, char **argv) {
  int i, option_char = 0;
  unsigned short port = 8888;
  unsigned short nworkerthreads = 1;
  unsigned short nsegments = 1;
  unsigned long segment_size = 1024;
  char *server = "s3.amazonaws.com/content.udacity-data.com";
  struct shm_info *shm_blk;

  if (signal(SIGINT, _sig_handler) == SIG_ERR){
    fprintf(stderr,"Can't catch SIGINT...exiting.\n");
    exit(EXIT_FAILURE);
  }

  if (signal(SIGTERM, _sig_handler) == SIG_ERR){
    fprintf(stderr,"Can't catch SIGTERM...exiting.\n");
    exit(EXIT_FAILURE);
  }

  // Parse and set command line arguments
  while ((option_char = getopt_long(argc, argv, "n:z:p:t:s:h", gLongOptions,
   NULL)) != -1) {
    switch (option_char) {
      case 'n': // num segments
        nsegments = atoi(optarg);
        break;
      case 'z': // size of segments
        segment_size = atol(optarg);
        break;
      case 'p': // listen-port
        port = atoi(optarg);
        break;
      case 't': // thread-count
        nworkerthreads = atoi(optarg);
        break;
      case 's': // file-path
        server = optarg;
        break;                                          
      case 'h': // help
        fprintf(stdout, "%s", USAGE);
        exit(0);
        break;       
      default:
        fprintf(stderr, "%s", USAGE);
        exit(1);
    }
  }
  
  /* SHM initialization...*/

  /*Initializing server*/
  gfserver_init(&gfs, nworkerthreads);

  /*Setting options*/
  gfserver_setopt(&gfs, GFS_PORT, port);
  gfserver_setopt(&gfs, GFS_MAXNPENDING, 10);
  gfserver_setopt(&gfs, GFS_WORKER_FUNC, handle_with_cache);

  steque_init(&segfds_q);
  /* Create the segments */
  for (i = 0; i < nsegments; i++) {
    shm_blk = malloc(sizeof(*shm_blk));
    snprintf(shm_blk->mem_name, sizeof(shm_blk->mem_name), "mem_%d", i);
    if (shm_unlink(shm_blk->mem_name) == 0) {
      fprintf(stdout, "Shared mem %s removed from system.\n",
        shm_blk->mem_name);
    }
    shm_blk->memfd = shm_open(shm_blk->mem_name, O_CREAT | O_RDWR | O_TRUNC,
        0777);
    if (shm_blk->memfd < 0) {
      perror("shm_open");
      exit(1);
    }
    ftruncate(shm_blk->memfd, segment_size);
    snprintf(shm_blk->sem1_name, sizeof(shm_blk->sem1_name), "sem_%d_1", i);
    if (sem_unlink(shm_blk->sem1_name) == 0) {
      fprintf(stdout, "Semaphore %s removed from system.\n",
        shm_blk->sem1_name);
    }
    snprintf(shm_blk->sem2_name, sizeof(shm_blk->sem2_name), "sem_%d_2", i);
    if (sem_unlink(shm_blk->sem2_name) == 0) {
      fprintf(stdout, "Semaphore %s removed from system.\n",
        shm_blk->sem2_name);
    }
    steque_push(&segfds_q, shm_blk);
  }
  for(i = 0; i < nworkerthreads; i++)
    gfserver_setopt(&gfs, GFS_WORKER_ARG, i, server);

  handle_with_cache_init(&segfds_q, segment_size, &segfds_q_mutex,
      &segfds_q_cond);

  /*Loops forever*/
  gfserver_serve(&gfs);
}
#include <stdlib.h>
#include <fcntl.h>
#include <curl/curl.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "gfserver.h"

struct MemoryStruct {
	char *memory;
	size_t size;
};


static size_t handle_recv_resp(void *ptr, size_t size, size_t nmemb,
		void *arg)
{
	struct MemoryStruct *chunk = (struct MemoryStruct *)arg;

	chunk->memory = realloc(chunk->memory, chunk->size + size * nmemb);
	memcpy(chunk->memory + chunk->size, ptr, size * nmemb);
	chunk->size += size * nmemb;

	return size * nmemb;
}

//Replace with an implementation of handle_with_curl and any other
//functions you may need.

ssize_t handle_with_curl(gfcontext_t *ctx, char *path, void* arg)
{
	char buffer[4096];
	char *url_base = arg;
	CURL *curl;
	CURLcode res;
	int status;
	CURLcode ccode;
	int write_len;
	struct MemoryStruct chunk;

	memset(&chunk, 0, sizeof(chunk));
	strcpy(buffer, url_base);
	strcat(buffer, path);

	curl = curl_easy_init();
	if (!curl) {
		return EXIT_FAILURE;
	}
	curl_easy_setopt(curl, CURLOPT_URL, buffer);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, handle_recv_resp);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
	/* Perform the request, res will get the return code */
	res = curl_easy_perform(curl);
	/* Check for errors */
	if (res != CURLE_OK) {
		fprintf(stderr, "curl_easy_perform() failed: %s\n",
		    curl_easy_strerror(res));
		return EXIT_FAILURE;
	}

	ccode = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	if (ccode != CURLE_OK) {
		return EXIT_FAILURE;
	}
	if (status != 200) {
		return gfs_sendheader(ctx, GF_FILE_NOT_FOUND, 0);
	}
	gfs_sendheader(ctx, GF_OK, chunk.size);
	write_len = gfs_send(ctx, chunk.memory, chunk.size);
	if (write_len != chunk.size) {
		fprintf(stderr, "handle_with_curl write error");
	}	
	/* always cleanup */	
	curl_easy_cleanup(curl);
	free(chunk.memory);
	return chunk.size;
}


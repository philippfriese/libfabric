/*
* Copyright (c) 2025 Philipp A. Friese, Technical University of Munich
*
* This software is available to you under a choice of one of two
* licenses.  You may choose to be licensed under the terms of the GNU
* General Public License (GPL) Version 2, available from the file
* COPYING in the main directory of this source tree, or the
* BSD license below:
*
*     Redistribution and use in source and binary forms, with or
*     without modification, are permitted provided that the following
*     conditions are met:
*
*      - Redistributions of source code must retain the above
*        copyright notice, this list of conditions and the following
*        disclaimer.
*
*      - Redistributions in binary form must reproduce the above
*        copyright notice, this list of conditions and the following
*        disclaimer in the documentation and/or other materials
*        provided with the distribution.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
* NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
* BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
* ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
* CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

#include <config.h>

#include <unistd.h>
#include <getopt.h>
#include <inttypes.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/wait.h>

#include <ofi_mem.h>
#include <ofi_list.h>
#include <ofi_lock.h>
#include <rdma/fi_errno.h>
#include <curl/curl.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <signal.h>

#ifdef __linux__
#include <sys/ioctl.h>
#include <sys/inotify.h>
#endif

#ifdef __APPLE__
#include <sys/mman.h>
#include <sys/mount.h>
#endif

#include <pthread.h>
#include <libgen.h>
#include <dirent.h>

#include <prov/hook/monitor/include/hook_monitor.h>

#define MON_SAMPLER_REMOTE_HEADERS_ENV "MON_SAMPLER_REMOTE_HEADERS"

#define MON_SAMPLER_PERF_LOG_PUSHER_OUTPATH_ENV "MON_SAMPLER_PERF_LOG_PUSHER_OUTPATH"
#define MON_SAMPLER_PERF_LOG_PUSHER_OUTPATH_DEFAULT "./log_pusher.csv"

#define MON_SAMPLER_PERF_LOG_SAMPLER_OUTPATH_ENV "MON_SAMPLER_PERF_LOG_SAMPLER_OUTPATH"
#define MON_SAMPLER_PERF_LOG_SAMPLER_OUTPATH_DEFAULT "./log_sampler.csv"

#define MON_SAMPLER_SPEED_LIMIT_KBS_ENV "MON_SAMPLER_SPEED_LIMIT_KBS"

#define MON_SAMPLER_PUSHER_MAX_WAIT_S_ENV "MON_SAMPLER_PUSHER_MAX_WAIT_S"
#define MON_SAMPLER_PUSHER_MAX_WAIT_S_DEFAULT 5

#define MON_SAMPLER_PUSHER_BUFFER_SIZE_KIB_DEFAULT 10240

#define INOFITY_ELEM_PER_BUF 32
#define INOFITY_BUF_SIZE (INOFITY_ELEM_PER_BUF*(sizeof(struct inotify_event) + NAME_MAX + 1))

#define PROVIDER_NAME_LEN 32

enum ms_formats {
	MS_CSV=0,
	MS_INFLUX=1
};

struct ms_opts {
	char *target_path;
	char *output;
	unsigned long watch_usec;
	enum ms_formats format;
	size_t buffer_size;
	bool use_stdout;
	bool verbose;
	bool should_log;
};

struct file_entry {
	char in_path[PATH_MAX];
	char out_path[PATH_MAX];
	struct monitor_mapped_data* share;
	struct dlist_entry list_entry;
	FILE *output;
	char hostname[HOST_NAME_MAX+1];
	int uid;
	int ppid;
	int pid;
	int64_t monitor_id;
	unsigned long job_id;
	char provider_name[PROVIDER_NAME_LEN];
	bool is_mapped;
	bool finalize;
	bool header_written;
};

#ifdef __linux__
struct inotify_entry {
	struct dlist_entry list_entry;
	int wd;
	char path[PATH_MAX];
};
#endif

struct ct_mon_sampler {
	struct ms_opts opts;
	struct monitor_data data[mon_api_size];
	mode_t target_mode;
	struct dlist_entry files;
	size_t num_files;

	/* performance log components */
	FILE *f_log_sampler;
	size_t procs_with_data;
	size_t serialized_bytes;
	size_t serialized_elements;

	/* inotify components */
#ifdef __linux__
	int inotify_fd;
	char inotify_buffer[INOFITY_BUF_SIZE] __attribute__((aligned(__alignof__(struct inotify_event))));
	struct dlist_entry inotifies;
#endif

	/* pusher components */
	struct ct_pusher *pusher;
	pthread_t pusher_thread;
	char *buffers[2];
	_Atomic uint8_t buffer_in_use;
	size_t buffer_num_elements;
	size_t buffer_offset;
	time_t last_push;
};

struct ct_pusher {
	CURL *curl_handle;
	struct curl_slist *curl_headers;
	ofi_mutex_t data_available_mutex;
	ofi_mutex_t in_transmission_mutex;
	char *remote_output;

	char *buffer;
	size_t buffer_size;
	size_t max_wait_s;
	FILE *f_log_pusher;

	long speed_limit_bs;
};


// Note: keep in-sync with prov/hook/monitor/include/hook_monitor.h
static const char *mon_functions[] = {
	"mon_recv",         "mon_recvv",        "mon_recvmsg",
	"mon_trecv",        "mon_trecvv",       "mon_trecvmsg",
	"mon_send",         "mon_sendv",        "mon_sendmsg",
	"mon_inject",       "mon_senddata",     "mon_injectdata",
	"mon_tsend",        "mon_tsendv",       "mon_tsendmsg",
	"mon_tinject",      "mon_tsenddata",    "mon_tinjectdata",
	"mon_read",         "mon_readv",        "mon_readmsg",
	"mon_write",        "mon_writev",       "mon_writemsg",
	"mon_inject_write", "mon_writedata",    "mon_inject_writedata",
	"mon_mr_reg",       "mon_mr_regv",      "mon_mr_regattr",
	"mon_cq_read",      "mon_cq_readfrom",  "mon_cq_readerr",
	"mon_cq_sread",     "mon_cq_sreadfrom", "mon_cq_ctx",
	"mon_cq_msg_tx",    "mon_cq_msg_rx",    "mon_cq_data_tx",
	"mon_cq_data_rx",   "mon_cq_tagged_tx", "mon_cq_tagged_rx",
};

static const char* mon_buckets[] = {
	"0_64",	    "64_512",  "512_1K", "1K_4K", "4K_64K",
	"64K_256K", "256K_1M", "1M_4M",	 "4M_UP",
};


static volatile sig_atomic_t running_sampler = 0, running_pusher = 0;

static void sigint_handler(int signal) {
	running_sampler = 0; // handle pusher thread via running_pusher in main thread
	fprintf(stdout, "Received SIGINT, will stop\n");
}

/*******************************************************************************
 *                         Utility Functions
 ******************************************************************************/

static struct timespec timespec_diff(struct timespec *a, struct timespec *b) {
	struct timespec result = {
		.tv_sec  = a->tv_sec  - b->tv_sec,
		.tv_nsec = a->tv_nsec - b->tv_nsec
	};
	if (result.tv_nsec < 0) {
		--result.tv_sec;
		result.tv_nsec += 1000000000L;
	}
	return result;
}

static inline double timespec_to_ms(struct timespec interval) {
	return ((double)interval.tv_sec * 1e3) + ((double)interval.tv_nsec / 1e6);
}

/*******************************************************************************
 *                         Metric Pusher
 ******************************************************************************/

static int pusher_init_curl(struct ct_pusher *pusher) {
	struct curl_slist *temp = NULL;
	char *envstr;
	CURLcode code;

	code = curl_global_init(CURL_GLOBAL_ALL);
	if (code != CURLE_OK)
		goto curl_error;

	pusher->curl_handle = curl_easy_init();
	if (pusher->curl_handle == NULL) {
		fprintf(stderr, "Could not create curl handle\n");
		return -EINVAL;
	}

	code = curl_easy_setopt(pusher->curl_handle, CURLOPT_URL,
		pusher->remote_output);
	if (code != CURLE_OK)
		goto curl_error;

	code = curl_easy_setopt(pusher->curl_handle, CURLOPT_TCP_KEEPALIVE, 1L);
	if (code != CURLE_OK)
		goto curl_error;

	pusher->curl_headers = curl_slist_append(pusher->curl_headers,
		"Content-Type: text/plain; charset=utf8");
	if (!pusher->curl_headers)
		return -EINVAL;

	envstr = getenv(MON_SAMPLER_REMOTE_HEADERS_ENV);
	if (envstr != NULL) {
		temp = curl_slist_append(pusher->curl_headers, envstr);
		if (!temp)
			return -EINVAL;
		pusher->curl_headers = temp;
	}

	code = curl_easy_setopt(pusher->curl_handle, CURLOPT_MAX_SEND_SPEED_LARGE,
		pusher->speed_limit_bs * 1000);
	if (code != CURLE_OK)
		goto curl_error;

	code = curl_easy_setopt(pusher->curl_handle, CURLOPT_HTTPHEADER,
		pusher->curl_headers);
	if (code != CURLE_OK)
		goto curl_error;

	return 0;

curl_error:
	fprintf(stderr, "pusher_init_curl: Failed to CURL: %i (%s)\n",
		code, curl_easy_strerror(code));
	return -EINVAL;
}

static int pusher_post_data(struct ct_pusher *pusher) {
	struct timespec t1, t2, tnow;
	long http_code = 0;
	CURLcode code;

	if (pusher->buffer_size == 0)
		return 0;

	code = curl_easy_setopt(pusher->curl_handle, CURLOPT_POSTFIELDS,
				pusher->buffer);
	if (code != CURLE_OK) goto curl_error;

	code = curl_easy_setopt(pusher->curl_handle, CURLOPT_POSTFIELDSIZE_LARGE,
				(curl_off_t)pusher->buffer_size);
	if (code != CURLE_OK) goto curl_error;

	if (clock_gettime(CLOCK_MONOTONIC, &t1) == -1) goto error;

	code = curl_easy_perform(pusher->curl_handle);
	if (code != CURLE_OK) goto curl_error;

	if (clock_gettime(CLOCK_MONOTONIC, &t2) == -1) goto error;

	code = curl_easy_getinfo(pusher->curl_handle, CURLINFO_RESPONSE_CODE, &http_code);
	if (code != CURLE_OK) goto curl_error;

	if (http_code > 299 || http_code < 200)
		fprintf(stderr, "DB returned code %li\n", http_code);

	if (pusher->f_log_pusher) {
		const double run_ms = timespec_to_ms(timespec_diff(&t2, &t1));

		if (clock_gettime(CLOCK_REALTIME, &tnow) == -1) goto error;
		fprintf(pusher->f_log_pusher, "%zu.%09zu,%zu,%f\n",
			tnow.tv_sec, tnow.tv_nsec,
			pusher->buffer_size,
			run_ms);
	}
	pusher->buffer_size = 0;

	return 0;

error:
	fprintf(stderr, "pusher_post_data: Encountered error: %s\n",
			strerror(errno));
	return -EINVAL;
curl_error:
	pusher->buffer_size = 0;
	fprintf(stderr, "pusher_post_data: Failed to CURL: %i (%s)\n",
			code, curl_easy_strerror(code));
	return -EINVAL;
}

// runs in pthread
static void* pusher_run(void* arg /* struct ct_pusher* */) {
	int ret;
	struct ct_pusher* pusher = (struct ct_pusher*)arg;

	while (running_pusher) {
		ret = ofi_mutex_lock(&pusher->data_available_mutex);
		if (ret != 0) goto error;

		ret = pusher_post_data(pusher);
		if (ret != 0) goto error;

		ret = ofi_mutex_unlock(&pusher->in_transmission_mutex);
		if (ret != 0) goto error;
	}

	return NULL;
error:
	fprintf(stderr, "Error pushing data: %s\n", strerror(-ret));
	return NULL;
}

/*******************************************************************************
 *                         Output Functions
 ******************************************************************************/

static int ms_serialize_csv(struct ct_mon_sampler *ct, struct file_entry *file,
	char *buffer, size_t *offset, size_t buffer_size) {
	int bytes_written;
	if (!file->header_written) {
		for (int i = 0; i < mon_api_size; i++) {
			for (int j = 0; j < MON_SIZE_MAX; j++) {
				bytes_written = snprintf(buffer + *offset, buffer_size - *offset,
					"%s_%s_c,%s_%s_s",
					mon_functions[i], mon_buckets[j],
					mon_functions[i], mon_buckets[j]);
				if (bytes_written >= buffer_size - *offset)
					return -EMSGSIZE;
				*offset += bytes_written;
				if (!(i + 1 == mon_api_size && j + 1 == MON_SIZE_MAX)){
					bytes_written = snprintf(buffer + *offset, buffer_size - *offset, ",");
					if (bytes_written >= buffer_size - *offset)
						return -EMSGSIZE;
					*offset += bytes_written;
				}
			}
		}
		bytes_written+= snprintf(buffer + *offset, buffer_size - *offset, "\n");
		if (bytes_written >= buffer_size - *offset)
			return -EMSGSIZE;
		*offset += bytes_written;
		file->header_written = true;
	}

	for (int i = 0; i < mon_api_size; i++) {
		for (int j = 0; j < MON_SIZE_MAX; j++) {
			bytes_written = snprintf(buffer + *offset, buffer_size - *offset,
				"%" PRIu64 ",%" PRIu64,
				ct->data[i].count[j], ct->data[i].sum[j]);
			if (bytes_written >= buffer_size - *offset)
				return -EMSGSIZE;
			*offset += bytes_written;
			if (!(i + 1 == mon_api_size && j + 1 == MON_SIZE_MAX)){
				bytes_written = snprintf(buffer + *offset, buffer_size - *offset, ",");
				if (bytes_written >= buffer_size - *offset)
					return -EMSGSIZE;
				*offset += bytes_written;
			}
		}
	}
	bytes_written = snprintf(buffer + *offset, buffer_size - *offset, "\n");
	if (bytes_written >= buffer_size - *offset)
		return -EMSGSIZE;
	*offset += bytes_written;
	return 0;
}

static int ms_serialize_influx(struct ct_mon_sampler *ct, struct file_entry *file,
	char* buffer, size_t *offset, size_t buffer_size) {
	struct timespec spec;
	int bytes_written;
	uint64_t now;
	clock_gettime(CLOCK_REALTIME, &spec);
	now = (uint64_t) spec.tv_sec * 1000000000L + (uint64_t) spec.tv_nsec;
	for(int i = 0; i < mon_api_size; i++) {
		for (int j = 0; j < MON_SIZE_MAX; j++) {
			if (ct->data[i].count[j] == 0 && ct->data[i].sum[j] == 0)
				continue;
			bytes_written = snprintf(
					buffer + *offset,
					buffer_size - *offset,
					"ofi_hook_monitor,function=%s,bucket=%s,"
					"pid=%i,ppid=%i,monitor_id=%li,job_id=%" PRIu64 ","
					"provider=%s,hostname=%s,uid=%i"
					" count=%" PRIu64 ",sum=%" PRIu64 " %" PRIu64 "\n",
					mon_functions[i], mon_buckets[j],
					file->pid, file->ppid,
					file->monitor_id, file->job_id,
					file->provider_name, file->hostname,
					file->uid,
					ct->data[i].count[j], ct->data[i].sum[j],
					now);
			if (bytes_written >= buffer_size - *offset)
				return -EMSGSIZE;
			*offset += bytes_written;
		}
	}
	return 0;
}

static int ms_serialize_data(struct ct_mon_sampler *ct, struct file_entry *file, size_t *bytes_written) {
	int ret = 0;
	char* buffer = ct->buffers[ct->buffer_in_use];
	size_t *buffer_offset = &ct->buffer_offset;
	const size_t old_buffer_offset = ct->buffer_offset;

	switch (ct->opts.format) {
		case MS_CSV:
			ret = ms_serialize_csv(ct, file, buffer, buffer_offset, ct->opts.buffer_size);
			break;
		case MS_INFLUX:
			ret = ms_serialize_influx(ct, file, buffer, buffer_offset, ct->opts.buffer_size);
			break;
		default:
			break;
	}
	if (ret != 0) {
		ct->buffer_offset = old_buffer_offset;
		ct->buffers[ct->buffer_in_use][old_buffer_offset] = '\0';
		return -ret;
	}
	*bytes_written = ct->buffer_offset - old_buffer_offset;
	ct->buffer_num_elements++;

	return 0;
}

static int ms_writeout_data(struct ct_mon_sampler *ct, struct file_entry *file,
	size_t bytes_written) {
	const char *buffer = ct->buffers[ct->buffer_in_use] + ct->buffer_offset - bytes_written;

	if (file->output != NULL) {
		if (fwrite(buffer, 1, bytes_written, file->output) < bytes_written)
			return -EINVAL;
		if (fsync(fileno(file->output)) != 0)
			return -errno;
	}
	if (ct->opts.use_stdout) {
		if (fwrite(buffer, 1, bytes_written, stdout) < bytes_written)
			return -EINVAL;
	}
	return 0;
}

static void ms_rotate_buffers(struct ct_mon_sampler *ct) {
	ct->buffer_in_use ^= 0b1;
	ct->buffer_offset = 0;
	ct->buffer_num_elements = 0;
	ct->buffers[ct->buffer_in_use][0] = '\0';
}

static int ms_post_data_pusher(struct ct_mon_sampler *ct) {
	int ret = 0;
	ct->last_push = time(NULL);
	// wait for pusher potentially still sending old serialize_buffer
	ret = ofi_mutex_trylock(&ct->pusher->in_transmission_mutex);
	if (ret != 0) {
		if (ret == EBUSY)
			fprintf(stderr, "Waiting for pusher\n");
		else
			goto error;
		ret = ofi_mutex_lock(&ct->pusher->in_transmission_mutex);
		if (ret == -1) goto error;
	}

	ct->pusher->buffer = ct->buffers[ct->buffer_in_use];
	ct->pusher->buffer_size = ct->buffer_offset;

	// inform pusher that new data is available
	ret = ofi_mutex_unlock(&ct->pusher->data_available_mutex);
	if (ret == -1) goto error;

	return ret;
error:
	fprintf(stderr, "Error posting data: %s\n", strerror(errno));
	return -errno;
}

static int ms_post_data(struct ct_mon_sampler *ct) {
	int ret = 0;

	if (running_pusher != true) {
		fprintf(stderr, "Pusher should be running, but is not\n");
		return -EINVAL;
	}

	if (ct->pusher &&
		(ct->pusher->max_wait_s == 0 ||
		time(NULL) - ct->last_push <= ct->pusher->max_wait_s))
		return 0;

	ret = ms_post_data_pusher(ct);
	if (ret != 0)
		return ret;

	ms_rotate_buffers(ct);
	return ret;
}

/*******************************************************************************
 *                         Resource Management Functions
 ******************************************************************************/

static int file_entry_match(struct dlist_entry *entry, const void *arg) {
	struct file_entry *entry_ptr;
	entry_ptr = container_of(entry, struct file_entry, list_entry);
	return (strncmp(arg, entry_ptr->in_path, PATH_MAX) == 0);
}

static int ms_remove_file_entry(struct ct_mon_sampler *ct, struct dlist_entry *entry) {
	int fn = 0;
	struct file_entry *file;
	file = container_of(entry, struct file_entry, list_entry);

	ct->num_files--;
	if (file->output != NULL && file->output != stdout) {
		fn = fileno(file->output);
		if (fn == -1) goto error;
		if (fsync(fn) == -1) goto error;
		if (fclose(file->output) == -1) goto error;
	}
	if (file->is_mapped)
		if (munmap(file->share, sizeof(struct monitor_mapped_data)) == -1)
			goto error;
	if (file->finalize)
		if (remove(file->in_path) == -1)
			goto error;

	dlist_remove(entry);
	free(file);
	return 0;
error:
	fprintf(stderr, "Error removing %s: %s\n",
		file->in_path, strerror(errno));
	dlist_remove(entry);
	free(file);
	return -1;
}

/* file entry management functions */
static int ms_create_file_entry(struct ct_mon_sampler *ct, struct file_entry *file) {
	char file_path[PATH_MAX], file_name[PATH_MAX];
	char *token, *remainder, *endptr;
	char format[16] = "";
	int fd;

	if (file->is_mapped)
		return 0;

	fd = open(file->in_path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "Could not open %s: %s\n", file->in_path, strerror(errno));
		return -ENOENT;
	}
	file->share = mmap(0, sizeof (struct monitor_mapped_data),
			   PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (file->share == MAP_FAILED) {
		fprintf(stderr, "Could not mmap %s: %s\n", file->in_path, strerror(errno));
		return -EINVAL;
	}
	file->is_mapped = true;

	strncpy(file_name, file->in_path, PATH_MAX);

	remainder = basename(file_name);
	token = strtok_r(remainder, "_", &remainder);
	file->ppid = (int)strtol(token, &endptr, 10);
	if (*endptr) goto error_strtol;

	token = strtok_r(remainder, "_", &remainder);
	file->pid = (int)strtol(token, &endptr, 10);
	if (*endptr) goto error_strtol;

	token = strtok_r(remainder, "_", &remainder);
	file->monitor_id = (int64_t)strtol(token, &endptr, 10);
	if (*endptr) goto error_strtol;

	token = strtok_r(remainder, "_", &remainder);
	file->job_id = strtol(token, &endptr, 10);
	if (*endptr) goto error_strtol;

	strncpy(file->provider_name, remainder, PROVIDER_NAME_LEN-1);

	strncpy(file_path, file->in_path, PATH_MAX);
	remainder = basename(dirname(file_path));
	strncpy(file->hostname, remainder, HOST_NAME_MAX);

	strncpy(file_path, file->in_path, PATH_MAX);
	remainder = basename(dirname(dirname(file_path)));
	token = strtok_r(remainder, "_", &remainder);
	file->uid = (int64_t)strtol(token, &endptr, 10);
	if (*endptr) goto error_strtol;



	switch (ct->opts.format) {
		case MS_CSV:
			snprintf(format, 16, "csv");
			break;
		case MS_INFLUX:
			snprintf(format, 16, "influx");
			break;
		default:
			break;
	}

	if (ct->opts.output != NULL && file->output == NULL) {
		// -o specified, prepare output file
		if (snprintf(file->out_path, PATH_MAX, "%s/%s.%s",
			ct->opts.output, basename(file->in_path), format) < 0) {
			fprintf(stderr, "Could not format out_path for file %s\n",
					file->in_path);
			return -EINVAL;
		}
		file->output = fopen(file->out_path, "a");
		if (file->output == NULL) {
			fprintf(stderr, "Could not open out_path %s: %s\n",
					file->out_path, strerror(errno));
			return -EINVAL;
		}

		// make sure file got created so that below stat-call works in all cases
		fsync(fileno(file->output));
		struct stat st;
		if (stat(file->out_path, &st) == -1) {
			fprintf(stderr, "Could not stat %s: %s\n",
					file->out_path, strerror(errno));
			return -EINVAL;
		}
		if (st.st_size > 0)
			file->header_written = true;

		if (ct->opts.verbose)
			fprintf(stderr, "Writing sampled data to %s\n", file->out_path);
	}
	ct->num_files++;

	return 0;

error_strtol:
	fprintf(stderr, "Warning: Could not parse input file %s: %s\n", file->in_path, strerror(errno));
	return -EINVAL;
}

static int ms_update_file_entries_dir(struct ct_mon_sampler *ct, char* path) {
	struct dirent *dir_entry;
	DIR *dr;
	char subdir[PATH_MAX];
	int ret = 0;

	dr = opendir(path);
	if (dr == NULL)
		goto error;

	while ((dir_entry = readdir(dr)) != NULL) {
		if (dir_entry->d_type == DT_DIR &&
			strcmp(dir_entry->d_name, ".") != 0 &&
			strcmp(dir_entry->d_name, "..") != 0 ) {
			if (snprintf(subdir, PATH_MAX, "%s/%s",
				path, dir_entry->d_name) < 0) {
				errno = EINVAL;
				goto error;
			}

			ret = ms_update_file_entries_dir(ct, subdir);
			if (ret != 0)
				goto error;
		}
		if (dir_entry->d_type != DT_REG)
			continue;

		char in_path[PATH_MAX];
		if (snprintf(in_path, PATH_MAX, "%s/%s",
			     path, dir_entry->d_name) < 0) {
			errno = EINVAL;
			goto error;
		}
		if (dlist_find_first_match(&ct->files, file_entry_match, in_path) == NULL) {
			struct file_entry *fentry = calloc(1, sizeof(struct file_entry));
			if (fentry == NULL) {
				errno = ENOMEM;
				goto error;
			}
			strncpy(fentry->in_path, in_path, PATH_MAX);
			int ret = ms_create_file_entry(ct, fentry);
			if (ret != 0) {
				errno = ret;
				goto error;
			}
			dlist_insert_after(&fentry->list_entry, &ct->files);
		}
	}
	closedir(dr);

	return 0;
error:
	fprintf(stderr, "Could not handle directory %s: %s\n", path, strerror(errno));
	closedir(dr);
	return -errno;
}

// open new target file(s) and close deleted files
static int ms_update_file_entries(struct ct_mon_sampler *ct) {
	int ret;
	struct dlist_entry *entry, *tmp;
	struct file_entry *fentry;

	if (S_ISREG(ct->target_mode) && dlist_empty(&ct->files)) {
		struct file_entry *fentry = calloc(1, sizeof(struct file_entry));
		if (fentry == NULL) {
			return -ENOMEM;
		}
		strncpy(fentry->in_path, ct->opts.target_path, PATH_MAX-1);
		ret = ms_create_file_entry(ct, fentry);
		if (ret != 0)
			return ret;
		dlist_insert_after(&fentry->list_entry, &ct->files);
	}
	else if (S_ISDIR(ct->target_mode)) {
		ret = ms_update_file_entries_dir(ct, ct->opts.target_path);
		if (ret != 0)
			return ret;
	}

	// check whether any files have to be deleted
	dlist_foreach_safe(&ct->files, entry, tmp) {
		fentry = container_of(entry, struct file_entry, list_entry);
		if (fentry->finalize) {
			ms_remove_file_entry(ct, entry);
			continue;
		}
		struct stat st;
		if (stat(fentry->in_path, &st) == -1) {
			ms_remove_file_entry(ct, entry);
			continue;
		}
	}

	return 0;
}

static void ms_cleanup(struct ct_mon_sampler *ct) {
	struct dlist_entry *entry, *tmp;
#ifdef __linux__
	struct inotify_entry *inotify_ptr;
#endif
	if (ct->pusher) {
		if (running_pusher) {
			running_pusher = 0;
			ms_post_data_pusher(ct);
			pthread_join(ct->pusher_thread, NULL);
			pusher_post_data(ct->pusher);
		}
		if (ct->pusher->f_log_pusher)
			if (fclose(ct->pusher->f_log_pusher) != 0)
				goto error;
		if (ct->pusher->curl_handle != NULL)
			curl_easy_cleanup(ct->pusher->curl_handle);
		if (ct->pusher->curl_headers)
			curl_slist_free_all(ct->pusher->curl_headers);
		curl_global_cleanup();
		free(ct->pusher);
	}

	if (ct->files.next != NULL)
		dlist_foreach_safe(&ct->files, entry, tmp)
			ms_remove_file_entry(ct, entry);

	if (ct->buffers[0] != NULL)
		free(ct->buffers[0]);
	if (ct->buffers[1] != NULL)
		free(ct->buffers[1]);

	if (ct->f_log_sampler)
		if (fclose(ct->f_log_sampler) != 0)
			goto error;

#ifdef __linux__
	if (ct->inotifies.next != NULL) {
		dlist_foreach_safe(&ct->inotifies, entry, tmp) {
			inotify_ptr = container_of(entry, struct inotify_entry, list_entry);
			dlist_remove(entry);
			free(inotify_ptr);
		}
	}
	if(close(ct->inotify_fd) == -1)
		goto error;
#endif
	return;

error:
	fprintf(stderr, "Error cleaning up: %u (%s)\n",
		errno, strerror(errno));
}

/*******************************************************************************
 *                         inotify functions
 ******************************************************************************/

#ifdef __linux__
static int inotify_entry_match(struct dlist_entry *entry, const void *arg) {
	struct inotify_entry *entry_ptr;
	entry_ptr = container_of(entry, struct inotify_entry, list_entry);
	return (*(int *)arg == entry_ptr->wd);
}

static int ms_add_inotify_dir(struct ct_mon_sampler *ct, char* path) {
	struct dirent *dir_entry;
	DIR *dr;
	char subdir[PATH_MAX];
	struct inotify_entry *ientry;

	ientry = calloc(1, sizeof(struct inotify_entry));
	if (ientry == NULL) {
		errno = ENOMEM;
		goto error;
	}
	strncpy(ientry->path, path, PATH_MAX-1);
	ientry->wd = inotify_add_watch(ct->inotify_fd, path,
		IN_CREATE|IN_DELETE|IN_DELETE_SELF|IN_ONLYDIR|IN_EXCL_UNLINK);
	if (ientry->wd == -1)
		goto error;

	dlist_insert_after(&ientry->list_entry, &ct->inotifies);

	dr = opendir(path);
	if (dr == NULL)
		goto error;

	while ((dir_entry = readdir(dr)) != NULL) {
		if (dir_entry->d_type == DT_DIR &&
			strcmp(dir_entry->d_name, ".") != 0 &&
			strcmp(dir_entry->d_name, "..") != 0 ) {
			if (snprintf(subdir, PATH_MAX, "%s/%s",
				path, dir_entry->d_name) < 0) {
				fprintf(stderr, "Error formatting path components %s, %s\n",
					path, dir_entry->d_name);
				goto cleanup;
			}
			ms_add_inotify_dir(ct, subdir);
		}
	}
	closedir(dr);

	return 0;
error:
	fprintf(stderr, "Error while adding inotify watch to dir %s: %s\n",
		path, strerror(errno));
cleanup:
	closedir(dr);
	return -errno;
}

static int ms_handle_inotify_event(struct ct_mon_sampler *ct,
	struct inotify_event *event) {
	int ret = 0;
	char f_path[PATH_MAX];
	struct stat st;

	struct inotify_entry *ientry_ptr;
	struct dlist_entry *i_entry, *f_entry;

	if (event->mask & IN_IGNORED)
		return 0;

	i_entry = dlist_find_first_match(&ct->inotifies, inotify_entry_match,
		&event->wd);
	if (i_entry == NULL) {
		fprintf(stderr, "Could not find inotify '%s'\n",
			event->name);
		return -EINVAL;
	}
	ientry_ptr = container_of(i_entry, struct inotify_entry, list_entry);
	if (snprintf(f_path, PATH_MAX, "%s/%s",
		ientry_ptr->path, event->name) < 0) {
		fprintf(stderr, "Could not format f_path for file %s\n",
				event->name);
		return -EINVAL;
	}
	f_entry = dlist_find_first_match(&ct->files, file_entry_match, f_path);

	if (event->mask & IN_CREATE) {
		if (stat(f_path, &st) == -1) {
			fprintf(stderr, "Could not stat %s: %s\n",
				f_path, strerror(errno));
			return -errno;
		}
		if (S_ISDIR(st.st_mode)) {
			ret = ms_add_inotify_dir(ct, f_path);
			if (ret != 0) {
				fprintf(stderr, "Error adding inotify to dir %s: %s\n",
					f_path, strerror(ret));
				return ret;
			}
			ret = ms_update_file_entries_dir(ct, f_path);
			if (ret != 0) {
				fprintf(stderr, "Error updating directory %s: %s",
					f_path, strerror(ret));
				return ret;
			}
		}
		else if (S_ISREG(st.st_mode) && f_entry == NULL) {
			struct file_entry *fentry = calloc(1, sizeof(struct file_entry));
			if (fentry == NULL)
				return -ENOMEM;
			strncpy(fentry->in_path, f_path, PATH_MAX);
			ret = ms_create_file_entry(ct, fentry);
			if (ret != 0)
				return ret;
			dlist_insert_after(&fentry->list_entry, &ct->files);
		}
	}
	else if (event->mask & IN_DELETE && f_entry != NULL)
		ms_remove_file_entry(ct, f_entry);
	else if (event->mask & IN_DELETE_SELF) {
		dlist_remove(&ientry_ptr->list_entry);
		free(ientry_ptr);
	}
	return ret;
}

static int ms_check_inotify_events(struct ct_mon_sampler *ct) {
	ssize_t available_bytes = 0;
	ssize_t handled_bytes = 0;
	size_t offset = 0;
	struct inotify_event *event;
	ssize_t read_bytes;

	if (ioctl(ct->inotify_fd, FIONREAD, &available_bytes) == -1) {
		fprintf(stderr, "Error in ioctl on inotify fd: %u (%s)\n",
		errno, strerror(errno));
		return errno;
	}

	while (handled_bytes < available_bytes) {
		read_bytes = read(ct->inotify_fd, ct->inotify_buffer, INOFITY_BUF_SIZE);
		if (read_bytes == -1) {
			fprintf(stderr, "Error reading from inotify_fd: %u (%s)\n",
				errno, strerror(errno));
			return errno;
		}

		offset = 0;
		while (offset < read_bytes) {
			event = (struct inotify_event*)(ct->inotify_buffer + offset);
			ms_handle_inotify_event(ct, event);
			offset += sizeof(struct inotify_event) + event->len;
		}
		handled_bytes += read_bytes;
	}
	return 0;
}
#endif

/*******************************************************************************
 *                         Init and geopt Functions
 ******************************************************************************/

static int ms_init_pusher(struct ct_mon_sampler *ct) {
	int ret = 0;
	unsigned long out;
	char *endptr;
	char *envstr;

	fprintf(stdout, "Starting pusher\n");

	envstr = getenv(MON_SAMPLER_PUSHER_MAX_WAIT_S_ENV);
	if (envstr != NULL) {
		out = strtoul(envstr, &endptr, 0);
		if (errno == ERANGE || *endptr != '\0' || envstr == endptr)
			goto error_strtol;
		ct->pusher->max_wait_s = out;
	} else {
		ct->pusher->max_wait_s = MON_SAMPLER_PUSHER_MAX_WAIT_S_DEFAULT;
	}

	ct->pusher->f_log_pusher = NULL;
	running_pusher = 1;
	ret = pusher_init_curl(ct->pusher);
	if (ret != 0) return ret;
	ret = ofi_mutex_init(&ct->pusher->data_available_mutex);
	if (ret != 0) return ret;
	ret = ofi_mutex_init(&ct->pusher->in_transmission_mutex);
	if (ret != 0) return ret;
	ret = pthread_create(&ct->pusher_thread, NULL, pusher_run, ct->pusher);
	if (ret != 0) return ret;
	ret = pthread_setname_np(ct->pusher_thread, "pusher");
	if (ret != 0) return ret;

	ct->pusher->speed_limit_bs = ct->opts.buffer_size;
	envstr = getenv(MON_SAMPLER_SPEED_LIMIT_KBS_ENV);
	if (envstr != NULL) {
		out = strtoul(envstr, &endptr, 0);
		if (errno == ERANGE || *endptr != '\0' || envstr == endptr)
			goto error_strtol;
		ct->pusher->speed_limit_bs = out * 1000;
	}

	return 0;

error_strtol:
	fprintf(stderr, "Could not convert %s: %s\n", envstr, strerror(errno));
	return -EINVAL;
}

static int ms_init_perf_log(struct ct_mon_sampler *ct) {
	char *fpath;
	char *env_str;

	if (ct->pusher) {
		env_str = getenv(MON_SAMPLER_PERF_LOG_PUSHER_OUTPATH_ENV);
		fpath = env_str != NULL ?
			env_str : MON_SAMPLER_PERF_LOG_PUSHER_OUTPATH_DEFAULT;
		ct->pusher->f_log_pusher = fopen(fpath, "w");
		if (ct->pusher->f_log_pusher == NULL) goto error;
		fprintf(ct->pusher->f_log_pusher,
			"time,buffer_size_bytes,upload_time_ms\n");
	}

	env_str = getenv(MON_SAMPLER_PERF_LOG_SAMPLER_OUTPATH_ENV);
	fpath = env_str != NULL ?
		env_str : MON_SAMPLER_PERF_LOG_SAMPLER_OUTPATH_DEFAULT;
	ct->f_log_sampler = fopen(fpath, "w");
	if (ct->f_log_sampler == NULL) goto error;
	fprintf(ct->f_log_sampler,
		"time,num_files,sample_time_ms,serialized_elements,"
		"serialized_bytes,procs_with_data\n");
	return 0;

error:
	fprintf(stderr, "Error opening file %s: %s\n", fpath, strerror(errno));
	return -errno;
}

static int ms_init_sampler(struct ct_mon_sampler *ct) {
	int ret = 0;
	struct stat st;

	fprintf(stdout, "Starting sampler\n");

	if (ct->opts.format != MS_INFLUX && ct->pusher != NULL)
		fprintf(stderr, "Did not specify influx output with remote server. "
		  "Make sure remote server accepts output format.\n");

	if (stat(ct->opts.target_path, &st) != 0) {
		fprintf(stderr, "Could not stat %s: %s\n",
			ct->opts.target_path, strerror(errno));
		return errno;
	}

	ct->target_mode = st.st_mode;
	if (S_ISDIR(ct->target_mode) && ct->opts.use_stdout) {
		fprintf(stderr, "Target is a directory, cannot use stdout as output!\n");
		return -ENOENT;
	}

	ct->buffers[0] = calloc(ct->opts.buffer_size, 1);
	if (ct->buffers[0] == NULL)
		goto error_buffers;
	ct->buffers[1] = calloc(ct->opts.buffer_size, 1);
	if (ct->buffers[0] == NULL)
		goto error_buffers;

	dlist_init(&ct->files);

	running_sampler = ct->opts.watch_usec > 0;

	if (ct->pusher) {
		ret = ms_init_pusher(ct);
		if (ret != 0) {
			fprintf(stderr, "Error initializing pusher: %s\n",
				strerror(ret));
			return ret;
		}
	}

	if (ct->opts.should_log)
		ms_init_perf_log(ct);

#ifdef __linux__
	dlist_init(&ct->inotifies);
	ct->inotify_fd = inotify_init();
	if (ct->inotify_fd == -1)
		return errno;

	if(S_ISDIR(ct->target_mode))
		ms_add_inotify_dir(ct, ct->opts.target_path);

	// inotify does not report on existing files; initialize file list manually
	ret = ms_update_file_entries(ct);
#endif
	return ret;

error_buffers:
	fprintf(stderr, "Could not allocate buffers: %s\n", strerror(errno));
	return -errno;
}

static void ms_usage(char *name) {
	fprintf(stderr, "Sampler for ofi_hook_monitor provider\n\n");

	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "  %s [OPTIONS] <target>\t"
			"start sampler on ofi_hook_monitor output target <target> "
			"(can be file or directory of files)\n",
			name);

	fprintf(stderr, "\nOptions:\n");
	fprintf(stderr, " %-20s %s\n", "-w <msec>",
			"watch file for changes, wait <msec> milliseconds between checks");
	fprintf(stderr, " %-20s %s\n", "-f <format>",
			"output format (CSV,INFLUX)");
	fprintf(stderr, " %-20s %s\n", "-o <outpath>",
			"output file");
	fprintf(stderr, " %-20s %s\n", "-s",
			"print output to stdout (only for file target)");
	fprintf(stderr, " %-20s %s\n", "-r <remote URL>",
			"URL of remote output database");
	fprintf(stderr, " %-20s %s (default: %i)\n", "-m <kib>",
			"size of pusher data buffer in KiB",
			MON_SAMPLER_PUSHER_BUFFER_SIZE_KIB_DEFAULT);
	fprintf(stderr, " %-20s %s\n", "-l", "enable performance logging of "
			"sampler (see Environment Variables for output settings)");
	fprintf(stderr, " %-20s %s\n", "-v", "enable verbose output");

	fprintf(stderr, "\n\n");
	fprintf(stderr, "Environment Variables:\n");
	fprintf(stderr, " %-20s \n   %s\n", MON_SAMPLER_REMOTE_HEADERS_ENV,
			"Headers as string for remote output pusher "
			"(default: ``, example: `Authorization: Bearer <token>`)");
	fprintf(stderr, " %-20s \n   %s (default: %s)\n",
			MON_SAMPLER_PERF_LOG_PUSHER_OUTPATH_ENV,
			"Path to output file for pusher performance log",
			MON_SAMPLER_PERF_LOG_PUSHER_OUTPATH_DEFAULT);
	fprintf(stderr, " %-20s \n   %s (default: %s)\n",
			MON_SAMPLER_PERF_LOG_SAMPLER_OUTPATH_ENV,
			"Path to output file for sampler performance log",
			MON_SAMPLER_PERF_LOG_SAMPLER_OUTPATH_DEFAULT);
	fprintf(stderr, "\n\n");

	fprintf(stderr, "Example:\n");
	fprintf(stderr, " fi_mon_sampler -w 1000 -f influx -r "
			"https://monitoring.database:1234 /dev/shm/ofi/1000/localhost\n");
	fprintf(stderr, " Note: Target path depends on ofi_hook_monitor settings. "
			"Default path is: /dev/shm/ofi/<uid>/<hostname>\n");
	fprintf(stderr, "       Use headers environment variable if database "
			"authorization is necessary.\n");
}

static int ms_parse_opts(int argc, char** argv, struct ct_mon_sampler *ct) {
	char *endptr;
	unsigned long long_out;
	int op;
	while ((op = getopt(argc, argv, "hvw:f:o:r:sm:l")) != -1) {
		switch (op) {
		case '?':
		case 'h':
			ms_usage(basename(argv[0]));
			return EXIT_FAILURE;
		case 'v':
			ct->opts.verbose = true;
			break;
		case 'w':
			long_out = strtoul(optarg, &endptr, 0);
			if (errno == ERANGE || *endptr != '\0' ||
			    optarg == endptr) {
				fprintf(stderr, "Invalid watch time '%s'\n",
					optarg);
				return -EINVAL;
			}
			ct->opts.watch_usec = long_out * 1000;
			break;
		case 'f':
			if (strcasecmp("csv", optarg) == 0)
				ct->opts.format = MS_CSV;
			else if (strcasecmp("influx", optarg) == 0)
				ct->opts.format = MS_INFLUX;
			else {
				fprintf(stderr, "Invalid format '%s'\n", optarg);
				return -EINVAL;
			}
			break;
		case 'o':
			ct->opts.output = optarg;
			break;
		case 's':
			ct->opts.use_stdout = true;
			break;
		case 'r':
			ct->pusher = calloc(sizeof(struct ct_pusher), 1);
			if (ct->pusher == NULL)
				return -ENOMEM;
			ct->pusher->remote_output = optarg;
			break;
		case 'l':
			ct->opts.should_log = true;
			break;
		case 'm':
			long_out = strtoul(optarg, &endptr, 0);
			if (errno == ERANGE || *endptr != '\0' ||
			    optarg == endptr) {
				fprintf(stderr, "Invalid size '%s'\n",
					optarg);
				return -EINVAL;
			}
			ct->opts.buffer_size = long_out*1024;
			break;
		default:
			break;
		}
	}

	if (optind == argc) {
		fprintf(stderr, "No target path specified!\n");
		ms_usage(argv[0]);
		return 0;
	}
	ct->opts.target_path = argv[optind];

	if (ct->opts.should_log && ct->pusher == NULL) {
		fprintf(stderr, "Specified benchmark (-l) without "
				"specifying remote target! "
				"Will only benchmark sampler.\n");
	}
	return 0;
}

/*******************************************************************************
 *                         Main Sampler Functions
 ******************************************************************************/

static int ms_extract_data(struct ct_mon_sampler *ct, struct file_entry *entry) {
	// check if data request is still pending
	if (entry->share->flags & 0b1)
		return -1;

	memcpy(ct->data, entry->share, sizeof (ct->data));

	// set request bit again
	entry->share->flags |= 0b1;

	// check if hook provider indicated end-of-data & that we should delete the file
	if ((entry->share->flags & 0b10) >> 1)
		entry->finalize = true;
	return 0;
}

static int ms_run(struct ct_mon_sampler *ct) {
	int ret = 0;
	size_t bytes_written;
	struct file_entry *file_ptr;
	struct dlist_entry *entry, *tmp;

#ifdef __linux__
	ret = ms_check_inotify_events(ct);
#else
	ret = ms_update_file_entries(ct);
#endif

	if (ret) {
		running_sampler = 0;
		return ret;
	}

	ct->procs_with_data = 0;
	ct->serialized_bytes = 0;
	ct->serialized_elements = 0;
	dlist_foreach_safe(&ct->files, entry, tmp) {
		file_ptr = container_of(entry, struct file_entry, list_entry);
		ret = ms_extract_data(ct, file_ptr);

		switch (ret) {
		case 0:
			ct->procs_with_data++;
			ret = ms_serialize_data(ct, file_ptr, &bytes_written);
			if (ret != 0) {
				if (ct->pusher) {
					fprintf(stderr, "Posting data due to buffer overflow\n");
					ret = ms_post_data_pusher(ct);
					if (ret != 0)
						return ret;
				}

				ct->serialized_bytes += ct->buffer_offset;
				ct->serialized_elements += ct->buffer_num_elements;
				ms_rotate_buffers(ct);
				ret = ms_serialize_data(ct, file_ptr, &bytes_written);
				if (ret != 0)
					return ret;
			}

			ret = ms_writeout_data(ct, file_ptr, bytes_written);
			if (ret != 0)
				return ret;

			if (ct->pusher) {
				ret = ms_post_data(ct);
				if (ret != 0)
					return ret;
			}
			break;
		case -1:
			ret = 0; // TODO: this happens when there is no data; handle single-shot mode logging better herer
		default:
			break;
		}
		if (file_ptr->finalize)
			ms_remove_file_entry(ct, entry);
	}

	ct->serialized_bytes += ct->buffer_offset;
	ct->serialized_elements += ct->buffer_num_elements;
	if (ct->pusher) {
		ret = ms_post_data(ct);
		if (ret != 0)
			return ret;
	}

	return ret;
}

int main(int argc, char **argv) {
	struct ct_mon_sampler ct = {
		.opts = {
			.buffer_size = MON_SAMPLER_PUSHER_BUFFER_SIZE_KIB_DEFAULT*1024,
			.watch_usec = 0,
			.verbose = false,
			.use_stdout =  false,
		},
		.last_push = time(NULL),
		.buffer_offset= 0,
		.buffer_in_use = 0,
	};
	int ret = 0;
	struct timespec t1, t2, tnow;
	struct sigaction saction = { .sa_handler = sigint_handler };

	double run_ms = 0;
	if (sigaction(SIGINT, &saction, NULL) == -1) goto error_errno;

	ret = ms_parse_opts(argc, argv, &ct);
	if (ret != 0) goto error;

	ret = ms_init_sampler(&ct);
	if (ret != 0) goto error;

	do {
		if (clock_gettime(CLOCK_MONOTONIC, &t1) == -1) goto error_errno;
		ret = ms_run(&ct);
		if (ret < 0) goto error;

		if (clock_gettime(CLOCK_MONOTONIC, &t2) == -1) goto error_errno;
		run_ms = timespec_to_ms(timespec_diff(&t2, &t1));

		if (ct.opts.verbose) {
			printf("Spent %8.2f ms / %6lu ms (%8.2f left) "
				"for %2zu files\n",
				run_ms, ct.opts.watch_usec / 1000,
				((double)ct.opts.watch_usec / 1000.) - run_ms,
				ct.num_files);
		}

		if (ct.opts.should_log) {
			if (clock_gettime(CLOCK_REALTIME, &tnow) == -1) goto error_errno;
			fprintf(ct.f_log_sampler,
				"%zu.%09zu,%zu,%f,%li,%li,%zu\n",
				tnow.tv_sec, tnow.tv_nsec, ct.num_files,
				run_ms,
				ct.serialized_elements,
				ct.serialized_bytes,
				ct.procs_with_data);
		}
		usleep((useconds_t)ct.opts.watch_usec);
		running_sampler &= ret == 0;
	} while (running_sampler);

	if (ret != 0) goto error;

	ms_cleanup(&ct);
	return -ret;

error_errno:
	ret = -errno;
error:
	fprintf(stderr, "Encountered error: %s\n", strerror(-ret));
	ms_cleanup(&ct);
	return -ret;
}

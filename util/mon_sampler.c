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

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/wait.h>

#include <ofi_mem.h>
#include <ofi_list.h>
#include <rdma/fi_errno.h>

#include <sys/stat.h>

#ifdef __linux__
#include <sys/ioctl.h>
#include <sys/inotify.h>
#endif

#ifdef __APPLE__
#include <sys/mman.h>
#endif

#include <libgen.h>
#include <dirent.h>
#include <prov/hook/monitor/include/hook_monitor.h>

#define INOFITY_ELEM_PER_BUF 32
#define INOFITY_BUF_SIZE (INOFITY_ELEM_PER_BUF*(sizeof(struct inotify_event) + NAME_MAX + 1))


static volatile sig_atomic_t running = 0;

static void signal_handler(int signal) {
	running = 0;
}

enum ms_formats {
	MS_CSV=0,
};

struct ms_opts {
	char *target_path;
	char *output;
	unsigned long watch_usec;
	enum ms_formats format;
};

struct file_entry {
	char in_path[PATH_MAX];
	char out_path[PATH_MAX];
	struct monitor_mapped_data* share;
	struct dlist_entry list_entry;
	FILE *output;
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

	/* inotify components */
#ifdef __linux__
	int inotify_fd;
	char inotify_buffer[INOFITY_BUF_SIZE] __attribute__((aligned(__alignof__(struct inotify_event))));
	struct dlist_entry inotifies;
#endif
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

/*******************************************************************************
 *                         Output Functions
 ******************************************************************************/

static int ms_write_csv(struct monitor_data data[mon_api_size], struct file_entry *file) {
	if (!file->header_written) {
		for(int i = 0; i < mon_api_size; i++) {
			for (int j = 0; j < MON_SIZE_MAX; j++) {
				fprintf(file->output, "%s_%s_c,%s_%s_s",
					mon_functions[i], mon_buckets[j],
					mon_functions[i], mon_buckets[j]);
				if (!(i+1 == mon_api_size
				      && j+1 == MON_SIZE_MAX))
					fprintf(file->output, ",");
			}

		}
		fprintf(file->output, "\n");
		file->header_written = true;
	}

	for(int i = 0; i < mon_api_size; i++) {
		for (int j = 0; j < MON_SIZE_MAX; j++) {
			fprintf(file->output, "%" PRIu64 ",%" PRIu64,
				data[i].count[j],
				data[i].sum[j]);
			if (!(i+1 == mon_api_size && j+1 == MON_SIZE_MAX))
				fprintf(file->output, ",");
		}
	}
	fprintf(file->output, "\n");

	return 0;
}

static void ms_output_data(struct ct_mon_sampler *ct,
			   struct file_entry *file) {
	switch (ct->opts.format) {
	case MS_CSV:
		ms_write_csv(ct->data, file);
		break;
	default:
		break;
	}
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

// file entry management functions
int ms_create_file_entry(struct ct_mon_sampler *ct, struct file_entry *file) {
	if (file->is_mapped) {
		return 0;
	}
	int fd = open(file->in_path, O_RDWR);
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

	char format[16] = "";
	switch (ct->opts.format) {
	case MS_CSV:
		snprintf(format, 16, "csv");
		break;
	default:
		break;
	}

	if (file->output == NULL) {
		if (snprintf(file->out_path, PATH_MAX, "%s/%s.%s", ct->opts.output,
			     basename(file->in_path), format) < 0) {
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

		fprintf(stderr, "Writing sampled data to %s\n", file->out_path);
	}
	return 0;
}

static int ms_update_file_entries_dir(struct ct_mon_sampler *ct, char* path) {
	struct dirent *dir_entry;
	DIR *dr;
	char subdir[PATH_MAX];
	int ret = 0;

	dr = opendir(path);
	if (dr == NULL) {
		if (errno == EACCES) {
			fprintf(stderr, "Could not access directory %s, will skip.\n", path);
			return 0;
		}

		goto error;
	}

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

	if (ct->files.next != NULL)
		dlist_foreach_safe(&ct->files, entry, tmp)
			ms_remove_file_entry(ct, entry);
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
		fprintf(stderr, "Could not allocate memory\n");
		return -ENOMEM;
	}
	strncpy(ientry->path, path, PATH_MAX-1);
	ientry->wd = inotify_add_watch(ct->inotify_fd, path, 
		IN_CREATE|IN_DELETE|IN_DELETE_SELF|IN_ONLYDIR|IN_EXCL_UNLINK);
	if (ientry->wd == -1) {
		fprintf(stderr, "Could not create inotify watch at %s: %s\n",
			path, strerror(errno));
		return -errno;
	}

	dlist_insert_after(&ientry->list_entry, &ct->inotifies);

	dr = opendir(path);
	if (dr == NULL) {
		fprintf(stderr, "Error opening directory %s: %s\n", 
			path, strerror(errno));
		goto error_cleanup;
	}

	while ((dir_entry = readdir(dr)) != NULL) {
		if (dir_entry->d_type == DT_DIR && 
			strcmp(dir_entry->d_name, ".") != 0 && 
			strcmp(dir_entry->d_name, "..") != 0 ) {
			if (snprintf(subdir, PATH_MAX, "%s/%s", 
				path, dir_entry->d_name) < 0) {
				fprintf(stderr, "Error formatting path components %s, %s\n",
					path, dir_entry->d_name);
				goto error_cleanup;
			}
			ms_add_inotify_dir(ct, subdir);
		} 
	}
	closedir(dr);

	return 0;
error_cleanup:
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
 *                         Data Extraction Function
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

/*******************************************************************************
 *                         Main Run Loop
 ******************************************************************************/

int ms_run(struct ct_mon_sampler *ct) {
	struct file_entry *file_ptr;
	struct dlist_entry *entry, *tmp;
	int ret;

#ifdef __linux__
	ret = ms_check_inotify_events(ct);
#else
	ret = ms_update_file_entries(ct);
#endif

	if (ret) {
		running = 0;
		return ret;
	}

	dlist_foreach_safe(&ct->files, entry, tmp) {
		file_ptr = container_of(entry, struct file_entry, list_entry);
		ret = ms_extract_data(ct, file_ptr);

		switch (ret) {
		case 0:
			ms_output_data(ct, file_ptr);
			break;
		case -1:
		default:
			break;
		}
		if (file_ptr->finalize)
			ms_remove_file_entry(ct, entry);
	}

	return 0;
}

/*******************************************************************************
*                         CLI: Usage and Options parsing
 ******************************************************************************/

static int ms_init_sampler(struct ct_mon_sampler *ct) {
	int ret = 0;
	struct stat st;

	if (stat(ct->opts.target_path, &st) != 0) {
		fprintf(stderr, "Could not stat %s: %s\n",
			ct->opts.target_path, strerror(errno));
		return errno;
	}
	
	ct->target_mode = st.st_mode;
	if (S_ISDIR(ct->target_mode) && ct->opts.output == NULL) {
		fprintf(stderr, "Target is a directory, cannot use stdout as output!\n");
		return -ENOENT;
	}
	dlist_init(&ct->files);

#ifdef __linux__
	dlist_init(&ct->inotifies);
	ct->inotify_fd = inotify_init();
	if (ct->inotify_fd == -1) 
		return errno;
	ms_add_inotify_dir(ct, ct->opts.target_path);

	// inotify does not report on existing files; initialize file list manually 
	ret = ms_update_file_entries(ct);
#endif
	return ret;
}

static void ms_usage(char *name) {
	fprintf(stderr, "Sampler for ofi_hook_monitor provider\n\n");

	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "  %s [OPTIONS] <file>\t"
			"start sampler on ofi_hook_monitor output file <file>\n",
		name);

	fprintf(stderr, "\nOptions:\n");
	fprintf(stderr, " %-20s %s\n", "-w <msec>",
		"watch file for changes, wait <msec> milliseconds between checks");
	fprintf(stderr, " %-20s %s\n", "-f <format>",
		"output format (CSV)");
	fprintf(stderr, " %-20s %s\n", "-o <outpath>",
		"output file (stdout if unset)");
}

static int ms_parse_opts(struct ct_mon_sampler *ct, int op, char *current_optarg) {
	char *endptr;
	unsigned long out;
	switch (op) {
	case 'w':
		out = strtoul(current_optarg, &endptr, 0);
		if (errno == ERANGE || *endptr != '\0' || current_optarg == endptr) {
			fprintf(stderr, "Invalid watch time '%s'\n",
				current_optarg);
			return -EINVAL;
		}
		ct->opts.watch_usec = out * 1000;
		break;
	case 'f':
		if ((strlen(current_optarg) == 3) &&
		    strncasecmp("csv", current_optarg, 3) == 0) {
			ct->opts.format = MS_CSV;
		} else {
			fprintf(stderr, "Invalid format '%s'\n",
				current_optarg);
			return -EINVAL;
		}
		break;
	case 'o':
		ct->opts.output = current_optarg;
		break;
	default:
		break;
	}
	return 0;
}

int main(int argc, char **argv) {
	int op, ret = EXIT_SUCCESS;
	struct ct_mon_sampler ct = {};

	while ((op = getopt(argc, argv, "hw:f:o:")) != -1) {
		switch (op) {
		default:
			ret = ms_parse_opts(&ct, op, optarg);
			if (ret != 0) {
				ms_usage(argv[0]);
				return ret;
			}
			break;
		case '?':
		case 'h':
			ms_usage(argv[0]);
			return EXIT_FAILURE;
		}
	}
	if (optind == argc) {
		fprintf(stderr, "No target path specified!\n");
		ms_usage(argv[0]);
		return 0;
	}
	ct.opts.target_path = argv[optind];
	ret = ms_init_sampler(&ct);
	
	signal(SIGINT, signal_handler);
	running = ct.opts.watch_usec > 0;

	do {
		ret = ms_run(&ct);
		usleep(ct.opts.watch_usec);
	} while (running);

	if (ret != 0)
		fprintf(stderr, "Error while running: %d\n", ret);

	ms_cleanup(&ct);
	return -ret;
}

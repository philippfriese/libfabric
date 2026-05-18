/*
 * Copyright (c) 2025 Philipp A. Friese, Technical University of Munich
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL); Version 2, available from the file
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

#ifndef _HOOK_MONITOR_H_
#define _HOOK_MONITOR_H_

#include "ofi_hook.h"
#include "ofi.h"
#include "uthash.h"

#define MON_IGNORE_SIZE  0
#define MON_TICK_MAX_DEFAULT 1024
#define MON_LINGER_DEFAULT 0
#define MON_BASEPATH_DEFAULT "/dev/shm/ofi"
#define MON_FILE_MODE_DEFAULT 0600
#define MON_DIR_MODE_DEFAULT 01700
#define MON_PROVIDER_NAME_MAX 32

#define MON_DESCRIPTOR_VERSION 0b1

#define MONITOR_APIS_CQ(DECL)  \
	DECL(mon_cq_read),  \
	DECL(mon_cq_readfrom),	 \
	DECL(mon_cq_readerr),  \
	DECL(mon_cq_sread),  \
	DECL(mon_cq_sreadfrom),  \
	DECL(mon_cq_ctx),  \
	DECL(mon_cq_msg_tx),  \
	DECL(mon_cq_msg_rx),  \
	DECL(mon_cq_data_tx),	\
	DECL(mon_cq_data_rx),	\
	DECL(mon_cq_tagged_tx),  \
	DECL(mon_cq_tagged_rx),    \
	DECL(mon_api_cq_size)

#define MONITOR_APIS_DOM(DECL)  \
	DECL(mon_mr_reg),  \
	DECL(mon_mr_regv),  \
	DECL(mon_mr_regattr),  \
	DECL(mon_api_dom_size)

// Note: keep in-sync with util/mon_sampler.c
enum mon_api_ep_counters {
	mon_recv,
	mon_recvv,
	mon_recvmsg,
	mon_trecv,
	mon_trecvv,
	mon_trecvmsg,
	mon_send,
	mon_sendv,
	mon_sendmsg,
	mon_inject,
	mon_senddata,
	mon_injectdata,
	mon_tsend,
	mon_tsendv,
	mon_tsendmsg,
	mon_tinject,
	mon_tsenddata,
	mon_tinjectdata,
	mon_read,
	mon_readv,
	mon_readmsg,
	mon_write,
	mon_writev,
	mon_writemsg,
	mon_inject_write,
	mon_writedata,
	mon_inject_writedata,
#if HOOK_MONITOR_COLL
	mon_barrier,
	mon_broadcast,
	mon_alltoall,
	mon_allreduce,
	mon_allgather,
	mon_reduce_scatter,
	mon_reduce,
	mon_scatter,
	mon_gather,
	mon_msg,
	mon_barrier2,
#endif
	mon_api_ep_size
};
enum mon_api_cq_counters {
	MONITOR_APIS_CQ(OFI_ENUM_VAL)
};
enum mon_api_dom_counters {
	MONITOR_APIS_DOM(OFI_ENUM_VAL)
};

// Keep size of monitor_mapped_data constant for all context types, allowing
// static mmap of struct monitor_mapped_data. Context type EP is largest with
// 3888 bytes, fitting in page cache used by tmpfs (recommended storage backend).
#define MON_DATA_SIZE (mon_api_ep_size)

enum mon_size_bucket {
	MON_SIZE_0_64 = 0,
	MON_SIZE_64_512,
	MON_SIZE_512_1K,
	MON_SIZE_1K_4K,
	MON_SIZE_4K_64K,
	MON_SIZE_64K_256K,
	MON_SIZE_256K_1M,
	MON_SIZE_1M_4M,
	MON_SIZE_4M_UP,
	MON_SIZE_MAX,
};

struct monitor_data {
	uint64_t count[MON_SIZE_MAX];
	uint64_t sum[MON_SIZE_MAX];
};

enum monitor_context_type {
	MON_CTX_TYPE_EP,
	MON_CTX_TYPE_CQ,
	MON_CTX_TYPE_DOM
};

struct monitor_descriptor {
	uint32_t version;
	size_t length;
	uint64_t job_id;
	pid_t ppid;
	pid_t pid;
	char provider_name[MON_PROVIDER_NAME_MAX];
	enum monitor_context_type context_type;
	uintptr_t context_id;
	uintptr_t provider_id;
	size_t num_child_providers;
	uintptr_t *child_provider_ids; // serialised as array in comm-file
};

struct monitor_mapped_data {
	struct monitor_data data[MON_DATA_SIZE];

	/* Synchronisation Flag
	 * bit 0    : data flush request
	 * bit 1    : termination finished
	 * bit 2-7  : reserved
	 */
	_Atomic uint8_t flags;
};

struct monitor_context {
	uintptr_t key;
	size_t api_size;

	// internal counter data
	struct monitor_data data[MON_DATA_SIZE];

	struct monitor_descriptor descriptor;

	// external counter data, shared via communication file
	struct monitor_mapped_data* share;

	/* Communication File Layout
	   0x0000-0x0f30 : struct monitor_mapped_data
	   0x0f30-end    : struct monitor_descriptor  */
	char shm_name[PATH_MAX];

	// current number of hooked API calls
	unsigned int tick;

	UT_hash_handle hh;
};

struct monitor_fabric {
	struct hook_fabric fabric_hook;

	const struct fi_provider *hprov;

	struct monitor_context *monitor_contexts;
};

struct monitor_environment {
	int linger;
	unsigned int tick_max;
	int file_mode;
	int dir_mode;
	char basepath[PATH_MAX];
};

#endif /* _HOOK_MONITOR_H_ */

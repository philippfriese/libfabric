/*
 * Copyright (c) 2018-2023 Intel Corporation. All rights reserved.
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

#define MON_IGNORE_SIZE  0
#define TICK_MAX_DEFAULT 1000

#define MONITOR_APIS(DECL)  \
	DECL(mon_recv),  \
	DECL(mon_recvv), \
	DECL(mon_recvmsg), \
	DECL(mon_trecv), \
	DECL(mon_trecvv), \
	DECL(mon_trecvmsg), \
	DECL(mon_send), \
	DECL(mon_sendv), \
	DECL(mon_sendmsg), \
	DECL(mon_inject),  \
	DECL(mon_senddata),  \
	DECL(mon_injectdata),  \
	DECL(mon_tsend),  \
	DECL(mon_tsendv),  \
	DECL(mon_tsendmsg),  \
	DECL(mon_tinject),	 \
	DECL(mon_tsenddata),  \
	DECL(mon_tinjectdata),  \
	DECL(mon_read),  \
	DECL(mon_readv),  \
	DECL(mon_readmsg),  \
	DECL(mon_write),  \
	DECL(mon_writev),  \
	DECL(mon_writemsg),  \
	DECL(mon_inject_write),  \
	DECL(mon_writedata),  \
	DECL(mon_inject_writedata),  \
	DECL(mon_mr_reg),  \
	DECL(mon_mr_regv),  \
	DECL(mon_mr_regattr),  \
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
	DECL(mon_cq_tagged_rx),  \
	DECL(mon_api_size)

enum profile_api_counters {
	MONITOR_APIS(OFI_ENUM_VAL)
};

#define MON_RX_API_START  mon_recv
#define MON_RX_API_END mon_trecvmsg
#define MON_TX_API_START  mon_send
#define MON_TX_API_END mon_tinjectdata
#define MON_RD_API_START  mon_read
#define MON_RD_API_END mon_readmsg
#define MON_WR_API_START  mon_write
#define MON_WR_API_END mon_inject_writedata
#define MON_CQ_API_START  mon_cq_msg_tx
#define MON_CQ_API_END mon_cq_tagged_rx
#define MON_MR_API_START  mon_mr_reg
#define MON_MR_API_END mon_mr_regattr

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
	MON_SIZE_MAX
};

struct monitor_data {
	uint64_t count[MON_SIZE_MAX];
	uint64_t sum[MON_SIZE_MAX];
};

struct monitor_context {
	const struct fi_provider *hprov;
	struct monitor_data data[mon_api_size];
	size_t tick;
	size_t tick_max;
	time_t last_sync;
	char* share;
	size_t share_size;
	char path_data[64];
};

struct monitor_fabric {
	struct hook_fabric fabric_hook;
	struct monitor_context mon_ctx;
};

void mon_report(const struct fi_provider *hprov,  struct monitor_data *data);

#endif /* _HOOK_MONITOR_H_ */

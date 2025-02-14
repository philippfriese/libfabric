/*
 * Copyright (c) 2018-2023 Intel Corporation. All rights reserved.
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
#include <stdio.h>
#include <string.h>

#include "ofi_str.h"
#include "hook_monitor.h"

#define MON_STR_LEN	64
#define MON_HMEM_IFACE_MAX	  FI_HMEM_SYNAPSEAI+1

#define MON_OUTPUT_FORMAT    " \t%-22s%-20s%-12s%-12s%-12s%-12s\n"

static const char *mon_api_name[] = {
	MONITOR_APIS(OFI_STR)
};

static char mon_api_disp_str[mon_api_size][MON_STR_LEN];
static char mon_size_str[mon_api_size][MON_STR_LEN];

static bool mon_disp_name_avail = false;

/* get suffix str,  from MON_SIZE_A_B  to "A_B" */
#define CASEENUMSUFFIX(SYM, prefix, len)  \
    case SYM:  { char *symstr = #SYM;  \
		         ofi_strncatf(buf, len, &symstr[prefix]); break; }

/* convert "mon_<ops>"  to  "fi_<ops>" for log name*/
static inline void
set_api_name(char *buf, size_t size, const char *api_str) {
	int len = strlen("mon_");
	ofi_strncatf(buf, size, "fi_");
	ofi_strncatf(buf, size, &api_str[len]);
}

/* convert "mon_cq_xxx_tx"  to cq_xxx (FI_SEND)
 * convert "mon_cq_xxx_rx"  to cq_xxx (FI_RECV)
 */
static inline void
set_cq_api_name(char *buf, size_t size, const char *api_str) {
	int len = strlen(api_str);
	int prefix = strlen("mon_");

	ofi_strncatf(buf, size, &api_str[prefix]);
	buf[strlen(buf)-3] = '\0';
	if (!strncmp(&api_str[len-3], "_tx", 3))
		ofi_strncatf(buf, size, " (FI_SEND)");
	else
		ofi_strncatf(buf, size, " (FI_RECV)");
}

// string name for enum mon_size_bucket
static char *mon_size_bucket_tostr(char *buf, size_t buf_size,
                                    enum mon_size_bucket bucket) {
	int prefix_len = strlen("MON_SIZE_");
	buf[0] = '\0';
	switch(bucket) {
	CASEENUMSUFFIX(MON_SIZE_0_64, prefix_len, buf_size);
	CASEENUMSUFFIX(MON_SIZE_64_512, prefix_len, buf_size);
	CASEENUMSUFFIX(MON_SIZE_512_1K, prefix_len, buf_size);
	CASEENUMSUFFIX(MON_SIZE_1K_4K, prefix_len, buf_size);
	CASEENUMSUFFIX(MON_SIZE_4K_64K, prefix_len, buf_size);
	CASEENUMSUFFIX(MON_SIZE_64K_256K, prefix_len, buf_size);
	CASEENUMSUFFIX(MON_SIZE_256K_1M, prefix_len, buf_size);
	CASEENUMSUFFIX(MON_SIZE_1M_4M, prefix_len, buf_size);
	CASEENUMSUFFIX(MON_SIZE_4M_UP, prefix_len, buf_size);
	default:
		break;
    }
    return buf;
}


static char *ratio_to_str(char *str, int len, uint64_t value, uint64_t total)
{
	snprintf(str, len, "%6.2f", 100.0 * value / total);
	return str;
}

static inline void
mon_log_data(const struct fi_provider *prov, char *api_str, char *name,
              uint64_t count, uint64_t size, uint64_t total_count,
              uint64_t total_amount)
{
	char str1[MON_STR_LEN] = {'\0'};
	char str2[MON_STR_LEN] = {'\0'};
	char str3[MON_STR_LEN] = {'\0'};
	char str4[MON_STR_LEN] = {'\0'};

	if (!total_amount)
		FI_TRACE(prov, FI_LOG_CORE, MON_OUTPUT_FORMAT, api_str, name,
		         ofi_tostr_count(str1, sizeof(str1), count), "-",
		         ratio_to_str(str3, sizeof(str3), count, total_count),
		         "-");
	else
		FI_TRACE(prov, FI_LOG_CORE, MON_OUTPUT_FORMAT, api_str, name,
		         ofi_tostr_count(str1, sizeof(str1), count),
		         ofi_tostr_size(str2, sizeof(str2), size),
		         ratio_to_str(str3, sizeof(str3), count, total_count),
		         ratio_to_str(str4, sizeof(str4), size, total_amount));
}

/*
 * This logs data in a table format.
 * title:  api_label  size_label count amount %count %amount
 * Data from "st" to "end" presents are accumulated as 100%
 * Only log title if there is a data entry.
 * Add a summary row on the top if there is more than one data entries.
 */
static void mon_log_apis(const struct fi_provider *prov,
                            char *api_label, char *size_label, char *group,
                            int num_size, struct monitor_data *data,
                            int st, int end, bool *with_title)
{
	uint64_t count = 0;
	uint64_t amount = 0;
	uint64_t left = 0;
	bool api_logged = false;
	char *api_str;
	char str[MON_STR_LEN] = {'\0'};
	int  entries = 0;

	for(int i = st; i <= end; i++) {
		for (int j = 0; j < num_size; j++) {
			if (data[i].count[j]) {
				count += data[i].count[j];
				amount += data[i].sum[j];
				entries++;
			}
		}
	}
	if (!entries)
		return;

	if (*with_title) {
		FI_TRACE(prov, FI_LOG_CORE, MON_OUTPUT_FORMAT, api_label, size_label,
		         "Count", "Amount", "% Count", "% Amount");
		*with_title = false;
	}

	left = count;
	if (entries > 1) {
		mon_log_data(prov, group, "Any", count, amount, count, amount);
	}
	for (int i = st; left && (i <= end); i++) {
		api_logged = false;
		for (int j = 0; left && (j < num_size); j++)  {
			if (!data[i].count[j])
				continue;
			api_str = (char *)((api_logged)? "" : mon_api_disp_str[i]);
			if (!data[i].sum[j]) { // for "revc" and cq with FI_SEND
				mon_log_data(prov, api_str, "Any",
				    data[i].count[j], 0, count, 0);
			} else if (!strncmp(group, "mr reg", 6)) {
				mon_log_data(prov, api_str,
				    fi_tostr_r(str, sizeof(str), &j, FI_TYPE_HMEM_IFACE),
				    data[i].count[j], data[i].sum[j], count, amount);
			} else {
				mon_log_data(prov, api_str, mon_size_str[j],
				    data[i].count[j], data[i].sum[j], count, amount);
			}
			api_logged = true;
			left -= data[i].count[j];
		}
	}
	FI_TRACE(prov, FI_LOG_CORE, "\n");
}

void mon_report(const struct fi_provider *prov,  struct monitor_data *data)
{
	bool with_title = true;

	// first generate api name for log
	if (! mon_disp_name_avail) {
		for (int i = 0; i < mon_api_size; i++) {
			mon_api_disp_str[i][0] = '\0';
			if ((i >= MON_CQ_API_START) && (i <= MON_CQ_API_END))
				set_cq_api_name(mon_api_disp_str[i],
				        sizeof(mon_api_disp_str[i]), mon_api_name[i]);
			else
				set_api_name(mon_api_disp_str[i],
				        sizeof(mon_api_disp_str[i]), mon_api_name[i]);
		}
		for (int i = 0; i < MON_SIZE_MAX; i++) {
			mon_size_str[i][0] = '\0';
			mon_size_bucket_tostr(mon_size_str[i],
			                       sizeof(mon_size_str[i]), i);
		}
		mon_disp_name_avail = true;
	}

	FI_TRACE(prov, FI_LOG_CORE, "  \tprov: %s\n", prov->name);
	struct monitor_context *ctx = container_of(prov, struct monitor_context, hprov);
	FI_TRACE(prov, FI_LOG_CORE, "  \tfi_provider: %p, monitor_context: %p\n", prov, ctx);

	with_title = true;
	mon_log_apis(prov, "XFER_API", "Size", "recv", 1,
	                data, MON_RX_API_START, MON_RX_API_END, &with_title);
	mon_log_apis(prov, "XFER_API", "Size", "send", MON_SIZE_MAX,
	                data, MON_TX_API_START, MON_TX_API_END, &with_title);
	mon_log_apis(prov, "XFER_API", "Size", "read", MON_SIZE_MAX,
	                data, MON_RD_API_START, MON_RD_API_END, &with_title);
	mon_log_apis(prov, "XFER_API", "Size", "write", MON_SIZE_MAX,
	                data, MON_WR_API_START, MON_WR_API_END, &with_title);

	with_title = true;
	mon_log_apis(prov, "CQ", "Size", "cq read", MON_SIZE_MAX,
	                data, MON_CQ_API_START, MON_CQ_API_END, &with_title);

	with_title = true;
	mon_log_apis(prov, "MR REG", "Iface", "mr reg", MON_HMEM_IFACE_MAX,
	                data, MON_MR_API_START, MON_MR_API_END, &with_title);
}

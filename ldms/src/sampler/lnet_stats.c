/**
 * Copyright (c) 2016,2018 National Technology & Engineering Solutions
 * of Sandia, LLC (NTESS). Under the terms of Contract DE-NA0003525 with
 * NTESS, the U.S. Government retains certain rights in this software.
 * Copyright (c) 2016,2018 Open Grid Computing, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the BSD-type
 * license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *      Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *      Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *
 *      Neither the name of Sandia nor the names of any contributors may
 *      be used to endorse or promote products derived from this software
 *      without specific prior written permission.
 *
 *      Neither the name of Open Grid Computing nor the names of any
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 *      Modified source versions must be plainly marked as such, and
 *      must not be misrepresented as being the original software.
 *
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * \file lnet.c
 * \brief /proc/?/lnet/stats data provider
 *
 * as of Lustre 2.5-2.8:
 * __proc_lnet_stats in
 * lustre-?/lnet/lnet/router_proc.c
 * generates the proc file as:
 *         len = snprintf(tmpstr, tmpsiz,
 *                     "%u %u %u %u %u %u %u "LPU64" "LPU64" "
 *                     LPU64" "LPU64,
 *                     ctrs->msgs_alloc, ctrs->msgs_max,
 *                     ctrs->errors,
 *                     ctrs->send_count, ctrs->recv_count,
 *                     ctrs->route_count, ctrs->drop_count,
 *                     ctrs->send_length, ctrs->recv_length,
 *                     ctrs->route_length, ctrs->drop_length);
 * where LPU64 is equivalent to uint64_t.
 */
#define _GNU_SOURCE
#include <inttypes.h>
#include <unistd.h>
#include <sys/errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <pthread.h>
#include "ldms.h"
#include "ldmsd.h"
#include "sampler_base.h"
#include <assert.h>
#define PROC_FILE_DEFAULT "/proc/sys/lnet/stats"

static char *procfile = NULL;
static ldms_set_t set = NULL;
static ldmsd_msg_log_f msglog;
#define SAMP "lnet_stats"

static int metric_offset;
static base_data_t base;

static const char * stat_name[] = {
	"msgs_alloc",
	"msgs_max",
	"errors",
	"send_count",
	"recv_count",
	"route_count",
	"drop_count",
	"send_length",
	"recv_length",
	"route_length",
	"drop_length",
};

#define NAME_CNT sizeof(stat_name)/sizeof(stat_name[0])

static uint64_t stats_val[NAME_CNT];

#define ROUTER_STAT_BUF_SIZE NAME_CNT*24

static char statsbuf[ROUTER_STAT_BUF_SIZE];

static int parse_err_cnt;

static int parse_stats()
{
	int i;
	for (i = 0; i < NAME_CNT; i++) {
		stats_val[i] = 0;
	}
	FILE *fp;
	fp = fopen(procfile, "r");
	if (!fp) {
		return ENOENT;
	}
	int rc;
	char *s = fgets(statsbuf, sizeof(statsbuf) - 1, fp);
	if (!s)
	{
		rc = EIO;
		goto err;
	}

	assert(NAME_CNT == 11); /* fix scanf call if this fails */
	int n = sscanf(statsbuf,
		"%" SCNu64 " "
		"%" SCNu64 " "
		"%" SCNu64 " "
		"%" SCNu64 " "
		"%" SCNu64 " "
		"%" SCNu64 " "
		"%" SCNu64 " "
		"%" SCNu64 " "
		"%" SCNu64 " "
		"%" SCNu64 " "
		"%" SCNu64 " " ,
		&(stats_val[0]),
		&(stats_val[1]),
		&(stats_val[2]),
		&(stats_val[3]),
		&(stats_val[4]),
		&(stats_val[5]),
		&(stats_val[6]),
		&(stats_val[7]),
		&(stats_val[8]),
		&(stats_val[9]),
		&(stats_val[10]));

	if (n < 11)
	{
		rc = EIO;
		goto err;
	}

	rc = 0;

err:
	fclose(fp);
	return rc;

}

static int create_metric_set(base_data_t base)
{
	int rc, i;
	ldms_schema_t schema;

	int parse_err = parse_stats();
	if (parse_err) {
		msglog(LDMSD_LERROR, "Could not parse the " SAMP " file "
				"'%s'\n", procfile);
		rc = parse_err;
		goto err;
	}


	schema = base_schema_new(base);
	if (!schema) {
		msglog(LDMSD_LERROR,
		       "%s: The schema '%s' could not be created, errno=%d.\n",
		       __FILE__, base->schema_name, errno);
		rc = errno;
		goto err;
	}

	/* Location of the first metric */
	metric_offset = ldms_schema_metric_count_get(schema);

	/*
	 * Process the file to define all the metrics.
	 */
	i = 0;
	for ( ; i < NAME_CNT; i++) {
		rc = ldms_schema_metric_add(schema, stat_name[i], LDMS_V_U64);
		if (rc < 0) {
			rc = ENOMEM;
			goto err;
		}
	}

	set = base_set_new(base);
	if (!set) {
		rc = errno;
		goto err;
	}

	return 0;

 err:

	return rc;
}

/**
 * check for invalid flags, with particular emphasis on warning the user about
 */
static int config_check(struct attr_value_list *kwl, struct attr_value_list *avl, void *arg)
{
	char *value;
	int i;

	char* deprecated[]={"set"};

	for (i = 0; i < (sizeof(deprecated)/sizeof(deprecated[0])); i++){
		value = av_value(avl, deprecated[i]);
		if (value){
			msglog(LDMSD_LERROR, SAMP ": config argument %s is obsolete.\n",
			       deprecated[i]);
			return EINVAL;
		}
	}

	return 0;
}

static const char *usage(struct ldmsd_plugin *self)
{
	return  "config name=" SAMP " [file=<proc_name>] " BASE_CONFIG_USAGE
		"    <proc_name>  The lnet proc file name if not "  PROC_FILE_DEFAULT "\n";
}

static int config(struct ldmsd_plugin *self, struct attr_value_list *kwl, struct attr_value_list *avl)
{
	void * arg = NULL;
	int rc;

	if (set) {
		msglog(LDMSD_LERROR, SAMP ": Set already created.\n");
		return EINVAL;
	}

	rc = config_check(kwl, avl, arg);
	if (rc != 0) {
		return rc;
	}

	char *pvalue = av_value(avl, "file");
	if (pvalue) {
		procfile = strdup(pvalue);
	} else {
		procfile = strdup(PROC_FILE_DEFAULT);
	}
	if (!procfile) {
		msglog(LDMSD_LERROR, SAMP ": config out of memory.\n");
		return ENOMEM;
	}

	base = base_config(avl, SAMP, SAMP, msglog);
	if (!base)
		return EINVAL;

	rc  = create_metric_set(base);
	if (rc) {
		msglog(LDMSD_LERROR, SAMP ": failed to create a metric set.\n");
		goto err;
	}

	return 0;

err:
	base_del(base);
	return rc;
}

static ldms_set_t get_set(struct ldmsd_sampler *self)
{
	return set;
}

static int sample(struct ldmsd_sampler *self)
{
	int rc = 0, i;
	int metric_no;
	union ldms_value v;

	if (!set) {
		msglog(LDMSD_LDEBUG, SAMP ": plugin not initialized\n");
		return EINVAL;
	}

	base_sample_begin(base);

	metric_no = metric_offset;
	int parse_err = parse_stats();
	if (parse_err) {
		if (parse_err_cnt < 2) {
			msglog(LDMSD_LERROR, SAMP "Could not parse the " SAMP
				 " file '%s'\n", procfile);
		}
		parse_err_cnt++;
		rc = parse_err;
		goto out;
	}
	for (i = 0; i < NAME_CNT; i++) {
		v.v_u64 = stats_val[i];
		ldms_metric_set(set, metric_no, &v);
		metric_no++;
	}
 out:
	base_sample_end(base);
	return rc;
}

static void term(struct ldmsd_plugin *self)
{
	if (base)
		base_del(base);
	base = NULL;
	if (procfile)
		free(procfile);
	if (set)
		ldms_set_delete(set);
	set = NULL;
}

static struct ldmsd_sampler lnet_stats_plugin = {
	.base = {
		.name = SAMP,
		.type = LDMSD_PLUGIN_SAMPLER,
		.term = term,
		.config = config,
		.usage = usage,
	},
	.get_set = get_set,
	.sample = sample,
};

struct ldmsd_plugin *get_plugin(ldmsd_msg_log_f pf)
{
	msglog = pf;
	set = NULL;
	return &lnet_stats_plugin.base;
}

/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2010-2016 Open Grid Computing, Inc. All rights reserved.
 * Copyright (c) 2010-2016 Sandia Corporation. All rights reserved.
 *
 * Under the terms of Contract DE-AC04-94AL85000, there is a non-exclusive
 * license for use of this work by or on behalf of the U.S. Government.
 * Export of this program may require a license from the United States
 * Government.
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

#include <unistd.h>
#include <inttypes.h>
#include <stdarg.h>
#include <getopt.h>
#include <stdlib.h>
#include <sys/errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/un.h>
#include <ctype.h>
#include <netdb.h>
#include <dlfcn.h>
#include <assert.h>
#include <libgen.h>
#include <glob.h>
#include <time.h>
#include <event2/thread.h>
#include <coll/rbt.h>
#include <coll/str_map.h>
#include <ovis_util/util.h>
#include "event.h"
#include "ldms.h"
#include "ldmsd.h"
#include "ldms_xprt.h"
#include "ldmsd_request.h"
#include "config.h"

#define REPLYBUF_LEN 4096
static char replybuf[REPLYBUF_LEN];
char myhostname[HOST_NAME_MAX+1];
pthread_t ctrl_thread = (pthread_t)-1;
pthread_t inet_ctrl_thread = (pthread_t)-1;
int muxr_s = -1;
int inet_sock = -1;
int inet_listener = -1;
char *sockname = NULL;

static int cleanup_requested = 0;
int bind_succeeded;

int ldmsd_oneshot_sample(char *plugin_name, char *ts, char *errstr);
extern void cleanup(int x, char *reason);

pthread_mutex_t host_list_lock = PTHREAD_MUTEX_INITIALIZER;
LIST_HEAD(host_list_s, hostspec) host_list;
LIST_HEAD(ldmsd_store_policy_list, ldmsd_store_policy) sp_list;
pthread_mutex_t sp_list_lock = PTHREAD_MUTEX_INITIALIZER;

#define LDMSD_PLUGIN_LIBPATH_MAX	1024
LIST_HEAD(plugin_list, ldmsd_plugin_cfg) plugin_list;

void ldmsd_config_cleanup()
{
	if (ctrl_thread != (pthread_t)-1) {
		void *dontcare;
		pthread_cancel(ctrl_thread);
		pthread_join(ctrl_thread, &dontcare);
	}

	if (inet_ctrl_thread != (pthread_t)-1) {
		void *dontcare;
		pthread_cancel(inet_ctrl_thread);
		pthread_join(inet_ctrl_thread, &dontcare);
	}

	if (muxr_s >= 0)
		close(muxr_s);
	if (sockname && bind_succeeded) {
		ldmsd_log(LDMSD_LINFO, "LDMS Daemon deleting socket "
						"file %s\n", sockname);
		unlink(sockname);
	}

	if (inet_listener >= 0)
		close(inet_listener);
	if (inet_sock >= 0)
		close(inet_sock);
}

int send_reply(int sock, struct sockaddr *sa, ssize_t sa_len,
	       char *msg, ssize_t msg_len)
{
	struct msghdr reply;
	struct iovec iov;

	reply.msg_name = sa;
	reply.msg_namelen = sa_len;
	iov.iov_base = msg;
	iov.iov_len = msg_len;
	reply.msg_iov = &iov;
	reply.msg_iovlen = 1;
	reply.msg_control = NULL;
	reply.msg_controllen = 0;
	reply.msg_flags = 0;
	sendmsg(sock, &reply, 0);
	return 0;
}

struct ldmsd_plugin_cfg *ldmsd_get_plugin(char *name)
{
	struct ldmsd_plugin_cfg *p;
	LIST_FOREACH(p, &plugin_list, entry) {
		if (0 == strcmp(p->name, name))
			return p;
	}
	return NULL;
}

struct ldmsd_plugin_cfg *new_plugin(char *plugin_name,
				char *errstr, size_t errlen)
{
	char library_name[LDMSD_PLUGIN_LIBPATH_MAX];
	char library_path[LDMSD_PLUGIN_LIBPATH_MAX];
	struct ldmsd_plugin *lpi;
	struct ldmsd_plugin_cfg *pi = NULL;
	char *pathdir = library_path;
	char *libpath;
	char *saveptr = NULL;
	char *path = getenv("LDMSD_PLUGIN_LIBPATH");
	void *d;

	if (!path)
		path = LDMSD_PLUGIN_LIBPATH_DEFAULT;

	strncpy(library_path, path, sizeof(library_path) - 1);

	while ((libpath = strtok_r(pathdir, ":", &saveptr)) != NULL) {
		pathdir = NULL;
		snprintf(library_name, sizeof(library_name), "%s/lib%s.so",
			 libpath, plugin_name);
		d = dlopen(library_name, RTLD_NOW);
		if (d != NULL) {
			break;
		}
	}

	if (!d) {
		char *dlerr = dlerror();
		snprintf(errstr, errlen, "Failed to load the plugin '%s'. "
				"dlerror: %s", plugin_name, dlerr);
		goto err;
	}

	ldmsd_plugin_get_f pget = dlsym(d, "get_plugin");
	if (!pget) {
		snprintf(errstr, errlen,
			"The library of '%s' is missing the get_plugin() "
						"function.", plugin_name);
		goto err;
	}
	lpi = pget(ldmsd_msg_logger);
	if (!lpi) {
		snprintf(errstr, errlen, "The plugin '%s' could not be loaded.",
								plugin_name);
		goto err;
	}
	pi = calloc(1, sizeof *pi);
	if (!pi)
		goto enomem;
	pthread_mutex_init(&pi->lock, NULL);
	pi->thread_id = -1;
	pi->handle = d;
	pi->name = strdup(plugin_name);
	if (!pi->name)
		goto enomem;
	pi->libpath = strdup(library_name);
	if (!pi->libpath)
		goto enomem;
	pi->plugin = lpi;
	pi->sample_interval_us = 1000000;
	pi->sample_offset_us = 0;
	pi->synchronous = 0;
	LIST_INSERT_HEAD(&plugin_list, pi, entry);
	return pi;
enomem:
	snprintf(errstr, errlen, "No memory");
err:
	if (pi) {
		if (pi->name)
			free(pi->name);
		if (pi->libpath)
			free(pi->libpath);
		free(pi);
	}
	return NULL;
}

void destroy_plugin(struct ldmsd_plugin_cfg *p)
{
	free(p->libpath);
	free(p->name);
	LIST_REMOVE(p, entry);
	dlclose(p->handle);
	free(p);
}

const char *prdcr_state_str(enum ldmsd_prdcr_state state)
{
	switch (state) {
	case LDMSD_PRDCR_STATE_STOPPED:
		return "STOPPED";
	case LDMSD_PRDCR_STATE_DISCONNECTED:
		return "DISCONNECTED";
	case LDMSD_PRDCR_STATE_CONNECTING:
		return "CONNECTING";
	case LDMSD_PRDCR_STATE_CONNECTED:
		return "CONNECTED";
	}
	return "BAD STATE";
}


const char *match_selector_str(enum ldmsd_name_match_sel sel)
{
	switch (sel) {
	case LDMSD_NAME_MATCH_INST_NAME:
		return "INST_NAME";
	case LDMSD_NAME_MATCH_SCHEMA_NAME:
		return "SCHEMA_NAME";
	}
	return "BAD SELECTOR";
}

void __process_info_prdcr(enum ldmsd_loglevel llevel)
{
	ldmsd_prdcr_t prdcr;
	ldmsd_log(llevel, "\n");
	ldmsd_log(llevel, "========================================================================\n");
	ldmsd_log(llevel, "%s\n", "Producers");
#ifdef LDMSD_UPDATE_TIME
	ldmsd_log(llevel, "%-20s %-20s %-8s %-12s %-10s %s\n",
		 "Name", "Host", "Port", "ConnIntrvl", "State", "Schdule update time(usec)");
#else /* LDMSD_UPDATE_TIME */
	ldmsd_log(llevel, "%-20s %-20s %-8s %-12s %s\n",
		 "Name", "Host", "Port", "ConnIntrvl", "State");
#endif /* LDMSD_UPDATE_TIME */
	ldmsd_log(llevel, "-------------------- -------------------- ---------- ---------- ----------\n");
	ldmsd_cfg_lock(LDMSD_CFGOBJ_PRDCR);
	for (prdcr = ldmsd_prdcr_first(); prdcr; prdcr = ldmsd_prdcr_next(prdcr)) {
#ifdef LDMSD_UPDATE_TIME
		ldmsd_log(llevel, "%-20s %-20s %-8hu %-12d %-10s %lf\n",
			 prdcr->obj.name, prdcr->host_name, prdcr->port_no,
			 prdcr->conn_intrvl_us,
			 prdcr_state_str(prdcr->conn_state),
			 prdcr->sched_update_time);
#else /* LDMSD_UPDATE_TIME */
		ldmsd_log(llevel, "%-20s %-20s %-8hu %-12d %s\n",
			 prdcr->obj.name, prdcr->host_name, prdcr->port_no,
			 prdcr->conn_intrvl_us,
			 prdcr_state_str(prdcr->conn_state));
#endif /* LDMSD_UPDATE_TIME */
		ldmsd_prdcr_lock(prdcr);
		ldmsd_prdcr_set_t prv_set;
#ifdef LDMSD_UPDATE_TIME
		ldmsd_log(llevel, "    %-32s %-20s %-10s %s\n",
			 "Instance Name", "Schema Name", "State", "Update time (usec)");
		for (prv_set = ldmsd_prdcr_set_first(prdcr); prv_set;
		     prv_set = ldmsd_prdcr_set_next(prv_set)) {
			ldmsd_log(llevel, "    %-32s %-20s %-10s %lf\n",
				 prv_set->inst_name,
				 prv_set->schema_name,
				 ldmsd_prdcr_set_state_str(prv_set->state),
				 prv_set->updt_duration);
		}
#else /* LDMSD_UPDATE_TIME */
		ldmsd_log(llevel, "    %-32s %-20s %s\n",
			 "Instance Name", "Schema Name", "State");
		for (prv_set = ldmsd_prdcr_set_first(prdcr); prv_set;
		     prv_set = ldmsd_prdcr_set_next(prv_set)) {
			ldmsd_log(llevel, "    %-32s %-20s %s\n",
				 prv_set->inst_name,
				 prv_set->schema_name,
				 ldmsd_prdcr_set_state_str(prv_set->state));
		}
#endif /* LDMSD_UPDATE_TIME */

		ldmsd_prdcr_unlock(prdcr);
	}
	ldmsd_cfg_unlock(LDMSD_CFGOBJ_PRDCR);
	ldmsd_log(llevel, "-------------------- -------------------- ---------- ---------- ----------\n");
}

void __process_info_updtr(enum ldmsd_loglevel llevel)
{
	char offset_s[15];
	ldmsd_updtr_t updtr;
	ldmsd_log(llevel, "\n");
	ldmsd_log(llevel, "========================================================================\n");
	ldmsd_log(llevel, "%s\n", "Updaters");
#ifdef LDMSD_UPDATE_TIME
	ldmsd_log(llevel, "%-20s %-14s %-14s %-10s %-10s %s\n",
		 "Name", "Update Intrvl", "Offset", "State", "Submitting time (usec)", "Update time (usec)");
#else /* LDMSD_UDPATE_TIME */
	ldmsd_log(llevel, "%-20s %-14s %-14s %s\n",
		 "Name", "Update Intrvl", "Offset", "State");
#endif /* LDMSD_UPDATE_TIME */

	ldmsd_log(llevel, "-------------------- -------------- -------------- ----------\n");
	ldmsd_cfg_lock(LDMSD_CFGOBJ_UPDTR);
	for (updtr = ldmsd_updtr_first(); updtr; updtr = ldmsd_updtr_next(updtr)) {
		if (updtr->updt_task_flags & LDMSD_TASK_F_SYNCHRONOUS)
			sprintf(offset_s, "%d", updtr->updt_offset_us);
		else
			sprintf(offset_s, "ASYNC");
#ifdef LDMSD_UPDATE_TIME
		ldmsd_log(llevel, "%-20s %-14d %-14s %-10s %lf %lf\n",
			 updtr->obj.name, updtr->updt_intrvl_us,
			 offset_s,
			 ldmsd_updtr_state_str(updtr->state),
			 updtr->sched_duration,
			 updtr->duration);
#else /* LDMSD_UPDATE_TIME */
		ldmsd_log(llevel, "%-20s %-14d %-14s %s\n",
			 updtr->obj.name, updtr->updt_intrvl_us,
			 offset_s,
			 ldmsd_updtr_state_str(updtr->state));
#endif /* LDMSD_UPDATE_TIME */

		ldmsd_updtr_lock(updtr);
		ldmsd_name_match_t match;
		ldmsd_log(llevel, "    Metric Set Match Specifications (empty == All)\n");
		ldmsd_log(llevel, "    %-10s %s\n", "Compare To", "Value");
		ldmsd_log(llevel, "    ----------------------------------------\n");
		for (match = ldmsd_updtr_match_first(updtr); match;
		     match = ldmsd_updtr_match_next(match)) {
			ldmsd_log(llevel, "    %-10s %s\n",
				 match_selector_str(match->selector),
				 match->regex_str);
		}
		ldmsd_log(llevel, "    ----------------------------------------\n");
		ldmsd_prdcr_ref_t ref;
		ldmsd_prdcr_t prdcr;
		ldmsd_log(llevel, "    Producers (empty == None)\n");
		ldmsd_log(llevel, "    %-10s %-10s %-10s %s\n", "Name", "Transport", "Host", "Port");
		ldmsd_log(llevel, "    ----------------------------------------\n");
		for (ref = ldmsd_updtr_prdcr_first(updtr); ref;
		     ref = ldmsd_updtr_prdcr_next(ref)) {
			prdcr = ref->prdcr;
			ldmsd_log(llevel, "    %-10s %-10s %-10s %hu\n",
				 prdcr->obj.name,
				 prdcr->xprt_name,
				 prdcr->host_name,
				 prdcr->port_no);
		}
		ldmsd_log(llevel, "    ----------------------------------------\n");
		ldmsd_updtr_unlock(updtr);
	}
	ldmsd_log(llevel, "-------------------- -------------- ----------\n");
	ldmsd_cfg_unlock(LDMSD_CFGOBJ_UPDTR);
}

void __process_info_strgp(enum ldmsd_loglevel llevel)
{
	ldmsd_strgp_t strgp;
	ldmsd_log(llevel, "\n");
	ldmsd_log(llevel, "========================================================================\n");
	ldmsd_log(llevel, "%s\n", "Storage Policies");
	ldmsd_log(llevel, "%-15s %-15s %-15s %-15s %-s\n",
		 "Name", "Container", "Schema", "Back End", "State");
	ldmsd_log(llevel, "--------------- --------------- --------------- --------------- --------\n");
	ldmsd_cfg_lock(LDMSD_CFGOBJ_STRGP);
	for (strgp = ldmsd_strgp_first(); strgp; strgp = ldmsd_strgp_next(strgp)) {
		ldmsd_log(llevel, "%-15s %-15s %-15s %-15s %-8s\n",
			 strgp->obj.name,
			 strgp->container, strgp->schema, strgp->plugin_name,
			 ldmsd_strgp_state_str(strgp->state));
		ldmsd_strgp_lock(strgp);
		ldmsd_name_match_t match;
		ldmsd_log(llevel, "    Producer Match Specifications (empty == All)\n");
		ldmsd_log(llevel, "    %s\n", "Name");
		ldmsd_log(llevel, "    ----------------------------------------\n");
		for (match = ldmsd_strgp_prdcr_first(strgp); match;
		     match = ldmsd_strgp_prdcr_next(match)) {
			ldmsd_log(llevel, "    %s\n", match->regex_str);
		}
		ldmsd_log(llevel, "    ----------------------------------------\n");

		ldmsd_log(llevel, "    Metrics (empty == All)\n");
		ldmsd_log(llevel, "    %s\n", "Name");
		ldmsd_log(llevel, "    ----------------------------------------\n");
		ldmsd_strgp_metric_t metric;
		for (metric = ldmsd_strgp_metric_first(strgp); metric;
		     metric = ldmsd_strgp_metric_next(metric)) {
			ldmsd_log(llevel, "    %s\n", metric->name);
		}
		ldmsd_log(llevel, "    ----------------------------------------\n");
		ldmsd_strgp_unlock(strgp);
	}
	ldmsd_log(llevel, "--------------- --------------- --------------- --------------- ---------------\n");
	ldmsd_cfg_unlock(LDMSD_CFGOBJ_STRGP);
}

/**
 * Return information about the state of the daemon
 */
int process_info(char *replybuf, struct attr_value_list *avl, struct attr_value_list *kwl)
{
	int llevel = LDMSD_LALL;

	char *name;
	name = av_value(avl, "name");
	if (name) {
		if (0 == strcmp(name, "prdcr")) {
			__process_info_prdcr(llevel);
		} else if (0 == strcmp(name, "updtr")) {
			__process_info_updtr(llevel);
		} else if (0 == strcmp(name, "strgp")) {
			__process_info_strgp(llevel);
		} else {
			snprintf(replybuf, REPLYBUF_LEN, "%dInvalid name '%s'. "
					"The choices are prdcr, updtr, strgp.",
					-EINVAL, name);
		}
		snprintf(replybuf, REPLYBUF_LEN, "0");
		return 0;
	}

	extern int ev_thread_count;
	extern pthread_t *ev_thread;
	extern int *ev_count;
	int i;
	struct hostspec *hs;
	int verbose = 0;
	char *vb = av_value(avl, "verbose");
	if (vb && (strcasecmp(vb, "true") == 0 ||
			strcasecmp(vb, "t") == 0))
		verbose = 1;

	ldmsd_log(llevel, "Event Thread Info:\n");
	ldmsd_log(llevel, "%-16s %s\n", "----------------", "------------");
	ldmsd_log(llevel, "%-16s %s\n", "Thread", "Task Count");
	ldmsd_log(llevel, "%-16s %s\n", "----------------", "------------");
	for (i = 0; i < ev_thread_count; i++) {
		ldmsd_log(llevel, "%-16p %d\n",
			 (void *)ev_thread[i], ev_count[i]);
	}

	ldmsd_log(llevel, "========================================================================\n");
	__process_info_prdcr(llevel);

	__process_info_updtr(llevel);

	__process_info_strgp(llevel);

	snprintf(replybuf, REPLYBUF_LEN, "0");
	return 0;
}

int ldmsd_compile_regex(regex_t *regex, const char *regex_str,
				char *errbuf, size_t errsz)
{
	memset(regex, 0, sizeof *regex);
	int rc = regcomp(regex, regex_str, REG_NOSUB);
	if (rc) {
		(void)regerror(rc, regex, errbuf, errsz);
	}
	return rc;
}

/*
 * Load a plugin
 */
int ldmsd_load_plugin(char *plugin_name, char *errstr, size_t errlen)
{
	struct ldmsd_plugin_cfg *pi = ldmsd_get_plugin(plugin_name);
	if (pi) {
		snprintf(errstr, errlen, "Plugin '%s' already loaded",
							plugin_name);
		return EEXIST;
	}
	pi = new_plugin(plugin_name, errstr, errlen);
	if (!pi)
		return -1;
	return 0;
}

/*
 * Destroy and unload the plugin
 */
int ldmsd_term_plugin(char *plugin_name)
{
	int rc = 0;
	struct ldmsd_plugin_cfg *pi;

	pi = ldmsd_get_plugin(plugin_name);
	if (!pi)
		return ENOENT;

	pthread_mutex_lock(&pi->lock);
	if (pi->ref_count) {
		rc = EINVAL;
		pthread_mutex_unlock(&pi->lock);
		goto out;
	}
	pi->plugin->term(pi->plugin);
	pthread_mutex_unlock(&pi->lock);
	destroy_plugin(pi);
out:
	return rc;
}

/*
 * Configure a plugin
 */
int ldmsd_config_plugin(char *plugin_name,
			struct attr_value_list *_av_list,
			struct attr_value_list *_kw_list)
{
	int rc = 0;
	struct ldmsd_plugin_cfg *pi;

	pi = ldmsd_get_plugin(plugin_name);
	if (!pi)
		return ENOENT;

	pthread_mutex_lock(&pi->lock);
	rc = pi->plugin->config(pi->plugin, _kw_list, _av_list);
	pthread_mutex_unlock(&pi->lock);
out:
	return rc;
}

/*
 * Assign user data to a metric
 */
int ldmsd_set_udata(const char *set_name, const char *metric_name,
						const char *udata_s)
{
	ldms_set_t set;
	set = ldms_set_by_name(set_name);
	if (!set)
		return ENOENT;

	char *endptr;
	uint64_t udata = strtoull(udata_s, &endptr, 0);
	if (endptr[0] != '\0')
		return EINVAL;

	int mid = ldms_metric_by_name(set, metric_name);
	if (mid < 0)
		return -ENOENT;
	ldms_metric_user_data_set(set, mid, udata);

	ldms_set_put(set);
	return 0;
}

int ldmsd_set_udata_regex(char *set_name, char *regex_str,
		char *base_s, char *inc_s, char *errstr, size_t errsz)
{
	int rc = 0;
	ldms_set_t set;
	set = ldms_set_by_name(set_name);
	if (!set) {
		snprintf(errstr, errsz, "Set '%s' not found.", set_name);
		return ENOENT;
	}

	char *endptr;
	uint64_t base = strtoull(base_s, &endptr, 0);
	if (endptr[0] != '\0') {
		snprintf(errstr, errsz, "User data base '%s' invalid.",
								base_s);
		return EINVAL;
	}

	int inc = 0;
	if (inc_s)
		inc = atoi(inc_s);

	regex_t regex;
	rc = ldmsd_compile_regex(&regex, regex_str, errstr, errsz);
	if (rc)
		return rc;

	int i;
	uint64_t udata = base;
	char *mname;
	for (i = 0; i < ldms_set_card_get(set); i++) {
		mname = (char *)ldms_metric_name_get(set, i);
		if (0 == regexec(&regex, mname, 0, NULL, 0)) {
			ldms_metric_user_data_set(set, i, udata);
			udata += inc;
		}
	}
	regfree(&regex);
	ldms_set_put(set);
	return 0;
}

int resolve(const char *hostname, struct sockaddr_in *sin)
{
	struct hostent *h;

	h = gethostbyname(hostname);
	if (!h) {
		printf("Error resolving hostname '%s'\n", hostname);
		return -1;
	}

	if (h->h_addrtype != AF_INET) {
		printf("Hostname '%s' resolved to an unsupported address family\n",
		       hostname);
		return -1;
	}

	memset(sin, 0, sizeof *sin);
	sin->sin_addr.s_addr = *(unsigned int *)(h->h_addr_list[0]);
	sin->sin_family = h->h_addrtype;
	return 0;
}

int process_oneshot_sample(char *replybuf, struct attr_value_list *av_list,
			   struct attr_value_list *kw_list)
{
	char *attr;
	char *plugin_name, *ts;
	char err_str[LEN_ERRSTR];

	attr = "name";
	plugin_name = av_value(av_list, attr);
	if (!plugin_name)
		goto einval;

	attr = "time";
	ts = av_value(av_list, attr);
	if (!ts)
		goto einval;

	int rc = ldmsd_oneshot_sample(plugin_name, ts, err_str);
	snprintf(replybuf, REPLYBUF_LEN, "%d%s", -rc, err_str);
	goto out;

einval:
	snprintf(replybuf, REPLYBUF_LEN, "%dThe attribute '%s' is required.\n",
								-EINVAL, attr);
out:
	return 0;
}

int process_exit(char *replybuf, struct attr_value_list *av_list,
					struct attr_value_list *kw_list)
{
	cleanup_requested = 1;
	/* set flag for bottom of message handler loops to check for quit. */
	ldmsd_log(LDMSD_LINFO, "User requested exit.\n");
	snprintf(replybuf, REPLYBUF_LEN, "0cleanup request received.\n");
	return 0;
}

extern uint32_t ldmsd_req_attr_str2id(const char *name);
/*
 * If both \c name and \c value are NULL, the end attribute is added to req_buf.
 * If \c name is NULL but \c value is not NULL, the attribute of type ATTR_STRING
 * is added to req_buf.
 * If \c name and \c value are not NULL, the attribute of the corresponding type
 * is added to req_buf.
 * Otherwise, EINVAL is returned.
 */
static int add_attr_from_attr_str(char *name, char *value, ldmsd_req_ctxt_t reqc)
{
	struct ldmsd_req_attr_s attr;

	if (!name && !value) {
		attr.discrim = 0;
		attr.attr_len = 0;
	} else if (name && !value) {
		/* The attribute value must be provided */
		return EINVAL;
	} else {
		attr.discrim = 1;
		/* Assume that the string av is NULL-terminated */
		attr.attr_len = strlen(value) + 1; /* +1 to include \0 */
		if (!name) {
			/* Caller wants the attribute id of ATTR_STRING */
			attr.attr_id = LDMSD_ATTR_STRING;
		} else {
			attr.attr_id = ldmsd_req_attr_str2id(name);
			if (attr.attr_id < 0) {
				ldmsd_log(LDMSD_LERROR,
					"Invalid attribute name '%s'\n", name);
				return EINVAL;
			}
		}
	}

	if (reqc->req_len - reqc->req_off <
			sizeof(struct ldmsd_req_attr_s) + attr.attr_len) {
		reqc->req_buf = realloc(reqc->req_buf, reqc->req_len * 2);
		if (!reqc->req_buf) {
			ldmsd_log(LDMSD_LERROR, "Out of memory\n");
			return ENOMEM;
		}
		reqc->req_len = reqc->req_len * 2;
	}

	memcpy(&reqc->req_buf[reqc->req_off], &attr, sizeof(attr));
	reqc->req_off += sizeof(attr);

	if (attr.attr_len) {
		memcpy(&reqc->req_buf[reqc->req_off], value, attr.attr_len);
		reqc->req_off += attr.attr_len;
	}

	return 0;
}

/* data_len is excluding the null character */
int print_config_error(struct ldmsd_req_ctxt *reqc, char *data, size_t data_len,
		int msg_flags)
{
	if (data_len + 1 > reqc->rep_len - reqc->rep_off) {
		reqc->rep_buf = realloc(reqc->rep_buf,
				reqc->rep_off + data_len + 1);
		if (!reqc->rep_buf) {
			ldmsd_log(LDMSD_LERROR, "Out of memory\n");
			return ENOMEM;
		}
		reqc->rep_len = reqc->rep_off + data_len + 1;
	}

	if (data) {
		memcpy(&reqc->rep_buf[reqc->rep_off], data, data_len);
		reqc->rep_off += data_len;
		reqc->rep_buf[reqc->rep_off] = '\0';
	}
	if (reqc->rep_off)
		ldmsd_log(LDMSD_LERROR, "%s\n", reqc->rep_buf);

	return 0;
}

void __get_attr_name_value(char *av, char **name, char **value)
{
	*name = av;
	*value = strchr(av, '=');
	**value = '\0';
	(*value)++;
}

extern void req_ctxt_tree_lock();
extern void req_ctxt_tree_unlock();
extern uint32_t ldmsd_req_str2id(const char *verb);
extern ldmsd_req_ctxt_t alloc_req_ctxt(struct req_ctxt_key *key);
extern void free_req_ctxt(ldmsd_req_ctxt_t rm);
extern int ldmsd_handle_request(ldmsd_req_hdr_t request, ldmsd_req_ctxt_t reqc);
int process_config_line(char *line)
{
	static uint32_t msg_no = 0;
	struct ldmsd_req_hdr_s request = {
			.marker = LDMSD_REQ_SOM_F | LDMSD_REQ_EOM_F,
			.flags = LDMSD_RECORD_MARKER,
			.msg_no = msg_no,
			.rec_len = 0,
			.code = -1,
	};
	struct req_ctxt_key key = {
			.msg_no = msg_no,
			.sock_fd = -1
	};
	msg_no++;

	char *_line, *ptr, *verb, *av, *name, *value, *tmp = NULL;
	int idx, rc = 0, attr_cnt = 0;
	uint32_t req_id;
	size_t tot_attr_len = 0;
	struct ldmsd_req_ctxt *reqc = NULL;

	_line = strdup(line);
	if (!_line) {
		ldmsd_log(LDMSD_LERROR, "Out of memory\n");
		return ENOMEM;
	}

	/* Get the request id */
	verb = _line;
	av = strchr(_line, ' ');
	*av = '\0';
	av++;

	request.code = ldmsd_req_str2id(verb);
	if ((request.code < 0) || (request.code == LDMSD_NOTSUPPORT_REQ)) {
		rc = ENOSYS;
		goto out;
	}

	/* Prepare the request context */
	req_ctxt_tree_lock();
	reqc = alloc_req_ctxt(&key);
	if (!reqc) {
		ldmsd_log(LDMSD_LERROR, "Out of memory\n");
		rc = ENOMEM;
		goto out;
	}
	memcpy(&reqc->rh, &request, sizeof(request));

	reqc->resp_handler = print_config_error;
	reqc->dest_fd = -1; /* We don't need the destination file descriptor here */

	/* TODO: Make this support environment variable */
	if (request.code == LDMSD_PLUGN_CONFIG_REQ) {
		size_t len = strlen(av);
		size_t cnt = 0;
		tmp = malloc(len);
		if (!tmp) {
			ldmsd_log(LDMSD_LERROR, "Out of memory\n");
			rc = ENOMEM;
			goto out;
		}
		av = strtok_r(av, " ", &ptr);
		while (av) {
			__get_attr_name_value(av, &name, &value);

			if (0 == strncmp(name, "name", 4)) {
				/* Find the name attribute */
				rc = add_attr_from_attr_str(name, value, reqc);
				if (rc)
					goto out;
			} else {
				/* Construct the other attribute into a ATTR_STRING */
				cnt += snprintf(&tmp[cnt], len - cnt, "%s=%s ",
							name, value);
			}
			av = strtok_r(NULL, " ", &ptr);
		}
		tmp[cnt-1] = '\0'; /* Replace the last ' ' with '\0' */
		rc = add_attr_from_attr_str(NULL, tmp, reqc);
		if (rc)
			goto out;

	} else {
		av = strtok_r(av, " ", &ptr);
		while (av) {
			__get_attr_name_value(av, &name, &value);
			rc = add_attr_from_attr_str(name, value, reqc);
			if (rc)
				goto out;
			av = strtok_r(NULL, " ", &ptr);
		}
	}

	/* Add the end attribute */
	rc = add_attr_from_attr_str(NULL, NULL, reqc);
	if (rc)
		goto out;
	req_ctxt_tree_unlock();
	rc = ldmsd_handle_request(&request, reqc);
out:
	if (reqc) {
		req_ctxt_tree_lock();
		free_req_ctxt(reqc);
		req_ctxt_tree_unlock();
	}

	if (tmp)
		free(tmp);
	free(_line);
	return rc;
}

int process_config_file(const char *path, int *errloc)
{
	int rc = 0;
	int lineno = 0;
	FILE *fin = NULL;
	char *buff = NULL;
	char *line;
	char *comment;
	ssize_t off = 0;
	size_t cfg_buf_len = LDMSD_MAX_CONFIG_STR_LEN;
	char *env = getenv("LDMSD_MAX_CONFIG_STR_LEN");
	if (env)
		cfg_buf_len = strtol(env, NULL, 0);
	fin = fopen(path, "rt");
	if (!fin) {
		rc = errno;
		goto cleanup;
	}
	buff = malloc(cfg_buf_len);
	if (!buff) {
		rc = errno;
		goto cleanup;
	}

next_line:
	line = fgets(buff + off, cfg_buf_len - off, fin);
	if (!line)
		goto cleanup;
	lineno++;

	comment = strchr(line, '#');

	if (comment) {
		*comment = '\0';
	}

	off = strlen(buff);
	while (off && isspace(line[off-1])) {
		off--;
	}

	if (!off) {
		/* empty string */
		off = 0;
		goto next_line;
	}

	buff[off] = '\0';

	if (buff[off-1] == '\\') {
		buff[off-1] = ' ';
		goto next_line;
	}

	line = buff;
	while (isspace(*line)) {
		line++;
	}

	if (!*line) {
		/* buff contain empty string */
		off = 0;
		goto next_line;
	}

	rc = process_config_line(line);
	if (rc) {
		ldmsd_log(LDMSD_LERROR, "Problem in line: %s\n", line);
		goto cleanup;
	}
	off = 0;

	goto next_line;

cleanup:
	if (fin)
		fclose(fin);
	if (buff)
		free(buff);
	*errloc = lineno;
	return rc;
}

int process_include(char *replybuf, struct attr_value_list *av_list,
					struct attr_value_list * kw_list)
{
	int rc;
	const char *path;
	path = av_name(kw_list, 1);
	if (!path)
		return EINVAL;
	int errloc = 0;
	rc = process_config_file(path, &errloc);
	return rc;
}

int process_env(char *replybuf, struct attr_value_list *av_list,
					struct attr_value_list * kw_list)
{
	int rc = 0;
	int i;
	for (i = 0; i < av_list->count; i++) {
		struct attr_value *v = &av_list->list[i];
		rc = setenv(v->name, v->value, 1);
		if (rc)
			return rc;
	}
	return 0;
}

int process_log_rotate(char *replybuf, struct attr_value_list *av_list,
					struct attr_value_list *kw_list)
{
	int rc = ldmsd_logrotate();
	if (rc)
		snprintf(replybuf, REPLYBUF_LEN, "%d Failed to rotate the log file", -rc);
	else
		snprintf(replybuf, REPLYBUF_LEN, "%d", -rc);
	return 0;
}

extern int
process_request(int fd, struct msghdr *msg, size_t msg_len);
void *ctrl_thread_proc(void *v)
{
	struct sockaddr_un sun = {.sun_family = AF_UNIX, .sun_path = ""};
	socklen_t sun_len = sizeof(sun);
	int sock;

	struct msghdr msg;
	struct iovec iov;
	unsigned char *lbuf;
	struct sockaddr_storage ss;
	size_t cfg_buf_len = LDMSD_MAX_CONFIG_STR_LEN;
	char *env = getenv("LDMSD_MAX_CONFIG_STR_LEN");
	if (env)
		cfg_buf_len = strtol(env, NULL, 0);
	lbuf = malloc(cfg_buf_len);
	if (!lbuf) {
		ldmsd_log(LDMSD_LERROR,
			  "Fatal error allocating %zu bytes for config string.\n",
			  cfg_buf_len);
		cleanup(1, "ctrl thread proc out of memory");
	}

loop:
	sock = accept(muxr_s, (void *)&sun, &sun_len);
	if (sock < 0) {
		ldmsd_log(LDMSD_LERROR, "Error %d failed to accept.\n", inet_sock);
		goto loop;
	}

	iov.iov_base = lbuf;
	do {
		struct ldmsd_req_hdr_s request;
		ssize_t msglen;
		ss.ss_family = AF_UNIX;
		msg.msg_name = &ss;
		msg.msg_namelen = sizeof(ss);
		iov.iov_len = sizeof(request);
		iov.iov_base = &request;
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = NULL;
		msg.msg_controllen = 0;
		msg.msg_flags = 0;
		msglen = recvmsg(sock, &msg, MSG_PEEK);
		if (msglen <= 0)
			break;
		if (cfg_buf_len < request.rec_len) {
			free(lbuf);
			lbuf = malloc(request.rec_len);
			if (!lbuf) {
				cfg_buf_len = 0;
				break;
			}
			cfg_buf_len = request.rec_len;
		}
		iov.iov_base = lbuf;
		iov.iov_len = request.rec_len;

		msglen = recvmsg(sock, &msg, MSG_WAITALL);
		if (msglen < request.rec_len)
			break;

		process_request(sock, &msg, msglen);
		if (cleanup_requested)
			break;
	} while (1);
	if (cleanup_requested) {
		/* Reset it to prevent deadlock in cleanup */
		ctrl_thread = (pthread_t)-1;
		cleanup(0,"user quit");
	}
	goto loop;
	return NULL;
}

int ldmsd_config_init(char *name)
{
	struct sockaddr_un sun;
	int ret;

	/* Create the control socket parsing structures */
	if (!name) {
		char *sockpath = getenv("LDMSD_SOCKPATH");
		if (!sockpath)
			sockpath = "/var/run";
		sockname = malloc(sizeof(LDMSD_CONTROL_SOCKNAME) + strlen(sockpath) + 2);
		if (!sockname) {
			ldmsd_log(LDMSD_LERROR, "Our of memory\n");
			return -1;
		}
		sprintf(sockname, "%s/%s", sockpath, LDMSD_CONTROL_SOCKNAME);
	} else {
		sockname = strdup(name);
	}

	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strncpy(sun.sun_path, sockname,
			sizeof(struct sockaddr_un) - sizeof(short));

	/* Create listener */
	muxr_s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (muxr_s < 0) {
		ldmsd_log(LDMSD_LERROR, "Error %d creating muxr socket.\n",
				muxr_s);
		return -1;
	}

	/* Bind to our public name */
	ret = bind(muxr_s, (struct sockaddr *)&sun, sizeof(struct sockaddr_un));
	if (ret < 0) {
		ldmsd_log(LDMSD_LERROR, "Error %d binding to socket "
				"named '%s'.\n", errno, sockname);
		return -1;
	}
	bind_succeeded = 1;

	ret = listen(muxr_s, 1);
	if (ret < 0) {
		ldmsd_log(LDMSD_LERROR, "Error %d listen to sock named '%s'.\n",
				errno, sockname);
	}

	ret = pthread_create(&ctrl_thread, NULL, ctrl_thread_proc, 0);
	if (ret) {
		ldmsd_log(LDMSD_LERROR, "Error %d creating "
				"the control pthread'.\n");
		return -1;
	}
	return 0;
}

void *inet_ctrl_thread_proc(void *args)
{
#if OVIS_LIB_HAVE_AUTH
	const char *secretword = (const char *)args;
#endif
	struct msghdr msg;
	struct iovec iov;
	unsigned char *lbuf;
	struct sockaddr_in sin;
	struct sockaddr_in rem_sin;
	socklen_t addrlen = sizeof(rem_sin);
	size_t cfg_buf_len = LDMSD_MAX_CONFIG_STR_LEN;
	char *env = getenv("LDMSD_MAX_CONFIG_STR_LEN");
	if (env)
		cfg_buf_len = strtol(env, NULL, 0);
	lbuf = malloc(cfg_buf_len);
	if (!lbuf) {
		ldmsd_log(LDMSD_LERROR,
			  "Fatal error allocating %zu bytes for config string.\n",
			  cfg_buf_len);
		cleanup(1, "inet ctrl thread out of memory");
	}
	iov.iov_base = lbuf;
loop:
	inet_sock = accept(inet_listener, (struct sockaddr *)&rem_sin, &addrlen);
	if (inet_sock < 0) {
		ldmsd_log(LDMSD_LERROR, "Error %d failed to setting up the config "
				"listener.\n", inet_sock);
		goto loop;
	}

#if OVIS_LIB_HAVE_AUTH

#include <string.h>
#include "ovis_auth/auth.h"

#define _str(x) #x
#define str(x) _str(x)
	struct ovis_auth_challenge auth_ch;
	int rc;
	if (secretword && secretword[0] != '\0') {
		uint64_t ch = ovis_auth_gen_challenge();
		char *psswd = ovis_auth_encrypt_password(ch, secretword);
		if (!psswd) {
			ldmsd_log(LDMSD_LERROR, "Failed to generate "
					"the password for the controller\n");
			goto loop;
		}
		size_t len = strlen(psswd) + 1;
		char *psswd_buf = malloc(len);
		if (!psswd_buf) {
			ldmsd_log(LDMSD_LERROR, "Failed to authenticate "
					"the controller. Out of memory");
			free(psswd);
			goto loop;
		}

		ovis_auth_pack_challenge(ch, &auth_ch);
		rc = send(inet_sock, (char *)&auth_ch, sizeof(auth_ch), 0);
		if (rc == -1) {
			ldmsd_log(LDMSD_LERROR, "Error %d failed to send "
					"the challenge to the controller.\n",
					errno);
			free(psswd_buf);
			free(psswd);
			goto loop;
		}
		rc = recv(inet_sock, psswd_buf, len - 1, 0);
		if (rc == -1) {
			ldmsd_log(LDMSD_LERROR, "Error %d. Failed to receive "
					"the password from the controller.\n",
					errno);
			free(psswd_buf);
			free(psswd);
			goto loop;
		}
		psswd_buf[rc] = '\0';
		if (0 != strcmp(psswd, psswd_buf)) {
			shutdown(inet_sock, SHUT_RDWR);
			close(inet_sock);
			free(psswd_buf);
			free(psswd);
			goto loop;
		}
		free(psswd);
		free(psswd_buf);
		int approved = 1;
		rc = send(inet_sock, (void *)&approved, sizeof(int), 0);
		if (rc == -1) {
			ldmsd_log(LDMSD_LERROR, "Error %d failed to send "
				"the init message to the controller.\n", errno);
			goto loop;
		}
	} else {
		/* Don't do authetication */
		auth_ch.hi = auth_ch.lo = 0;
		rc = send(inet_sock, (char *)&auth_ch, sizeof(auth_ch), 0);
		if (rc == -1) {
			ldmsd_log(LDMSD_LERROR, "Error %d failed to send "
					"the greeting to the controller.\n",
					errno);
			goto loop;
		}
	}
#else /* OVIS_LIB_HAVE_AUTH */
	uint64_t greeting = 0;
	int rc = send(inet_sock, (char *)&greeting, sizeof(uint64_t), 0);
	if (rc == -1) {
		ldmsd_log(LDMSD_LERROR, "Error %d failed to send "
				"the greeting to the controller.\n",
				errno);
		goto loop;
	}
#endif /* OVIS_LIB_HAVE_AUTH */
	do {
		struct ldmsd_req_hdr_s request;
		ssize_t msglen;
		sin.sin_family = AF_INET;
		msg.msg_name = &sin;
		msg.msg_namelen = sizeof(sin);
		iov.iov_len = sizeof(request);
		iov.iov_base = &request;
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = NULL;
		msg.msg_controllen = 0;
		msg.msg_flags = 0;
		/* Read the message header */
		msglen = recvmsg(inet_sock, &msg, MSG_PEEK);
		if (msglen <= 0)
			break;
		if (cfg_buf_len < request.rec_len) {
			free(lbuf);
			lbuf = malloc(request.rec_len);
			if (!lbuf) {
				cfg_buf_len = 0;
				break;
			}
			cfg_buf_len = request.rec_len;
		}

		iov.iov_base = lbuf;
		iov.iov_len = request.rec_len;
		msglen = recvmsg(inet_sock, &msg, MSG_WAITALL);
		if (msglen < request.rec_len)
			break;

		process_request(inet_sock, &msg, msglen);
		if (cleanup_requested)
			break;

	} while (1);
	ldmsd_log(LDMSD_LINFO,
		  "Closing configuration socket. cfg_buf_len %d\n",
		  cfg_buf_len);
	close(inet_sock);
	inet_sock = -1;
	if (cleanup_requested) {
		/* Reset it to prevent deadlock in cleanup */
		inet_ctrl_thread = (pthread_t)-1;
		cleanup(0,"user quit");
		return NULL;
	}

	goto loop;
	return NULL;
}

int ldmsd_inet_config_init(const char *port, const char *secretword)
{
	int rc;
	struct sockaddr_in sin;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(atoi(port));

	inet_listener = socket(AF_INET, SOCK_STREAM, 0);
	if (inet_listener < 0) {
		ldmsd_log(LDMSD_LERROR, "Error %d creating socket on port "
				"'%s'\n", errno, port);
		return errno;
	}

	/* Bind to our public name */
	rc = bind(inet_listener, (struct sockaddr *)&sin, sizeof(sin));
	if (rc < 0) {
		ldmsd_log(LDMSD_LERROR, "Error %d binding to socket on port '%s'.\n",
						errno, port);
		goto err;
	}

	rc = listen(inet_listener, 10);
	if (rc) {
		ldmsd_log(LDMSD_LERROR, "Error %d failed to setting up the config "
				"listener.\n", rc);
		goto err;
	}

	rc = pthread_create(&inet_ctrl_thread, NULL, inet_ctrl_thread_proc,
			(void *)secretword);
	if (rc) {
		ldmsd_log(LDMSD_LERROR, "Error %d creating the control pthread'.\n");
		goto err;
	}
	return 0;
err:
	close(inet_listener);
	return rc;
}



const char * blacklist[] = {
	"liblustre_sampler.so",
	"libzap.so",
	"libzap_rdma.so",
	"libzap_sock.so",
	NULL
};

#define APP "ldmsd"

/* Dump plugin names and usages (where available) before ldmsd redirects
 * io. Loads and terms all plugins, which provides a modest check on some
 * coding and deployment issues.
 * \param plugname: list usage only for plugname. If NULL, list all plugins.
 */
int ldmsd_plugins_usage(const char *plugname)
{
	struct stat buf;
	glob_t pglob;

	char *path = getenv("LDMSD_PLUGIN_LIBPATH");
	if (!path)
		path = PLUGINDIR;

	if (! path ) {
		fprintf(stderr, "%s: need plugin path input.\n", APP);
		fprintf(stderr, "Did not find env(LDMSD_PLUGIN_LIBPATH).\n");
		return EINVAL;
	}

	if (stat(path, &buf) < 0) {
		int err = errno;
		fprintf(stderr, "%s: unable to stat library path %s (%d).\n",
			APP, path, err);
		return err;
	}

	int rc = 0;

	const char *match1 = "/lib";
	const char *match2 = ".so";
	size_t patsz = strlen(path) + strlen(match1) + strlen(match2) + 2;
	if (plugname) {
		patsz += strlen(plugname);
	}
	char *pat = malloc(patsz);
	if (!pat) {
		fprintf(stderr, "%s: out of memory?!\n", APP);
		rc = ENOMEM;
		goto out;
	}
	snprintf(pat, patsz, "%s%s%s%s", path, match1,
		(plugname ? plugname : "*"), match2);
	int flags = GLOB_ERR |  GLOB_TILDE | GLOB_TILDE_CHECK;

	int err = glob(pat, flags, NULL, &pglob);
	switch(err) {
	case 0:
		break;
	case GLOB_NOSPACE:
		fprintf(stderr, "%s: out of memory!?\n", APP);
		rc = ENOMEM;
		break;
	case GLOB_ABORTED:
		fprintf(stderr, "%s: error reading %s\n", APP, path);
		rc = 1;
		break;
	case GLOB_NOMATCH:
		fprintf(stderr, "%s: no libraries in %s\n", APP, path);
		rc = 1;
		break;
	default:
		fprintf(stderr, "%s: unexpected glob error for %s\n", APP, path);
		rc = 1;
		break;
	}
	if (err)
		goto out2;

	size_t i = 0;
	if (pglob.gl_pathc > 0) {
		printf("LDMSD plugins in %s : \n", path);
	}
	for ( ; i  < pglob.gl_pathc; i++) {
		char * library_name = pglob.gl_pathv[i];
		char *tmp = strdup(library_name);
		if (!tmp) {
			rc = ENOMEM;
			goto out2;
		} else {
			char *b = basename(tmp);
			int j = 0;
			int blacklisted = 0;
			while (blacklist[j]) {
				if (strcmp(blacklist[j], b) == 0) {
					blacklisted = 1;
					break;
				}
				j++;
			}
			if (blacklisted)
				goto next;
		       	/* strip lib prefix and .so suffix*/
			b+= 3;
			char *suff = rindex(b, '.');
			*suff = '\0';
			char err_str[LEN_ERRSTR];
			if (ldmsd_load_plugin(b, err_str, LEN_ERRSTR)) {
				fprintf(stderr, "Unable to load plugin %s: %s\n",
					b, err_str);
				goto next;
			}
			struct ldmsd_plugin_cfg *pi = ldmsd_get_plugin(b);
			if (!pi) {
				fprintf(stderr, "Unable to get plugin %s\n",
					b);
				goto next;
			}
			const char *ptype;
			switch (pi->plugin->type) {
			case LDMSD_PLUGIN_OTHER:
				ptype = "OTHER";
				break;
			case LDMSD_PLUGIN_STORE:
				ptype = "STORE";
				break;
			case LDMSD_PLUGIN_SAMPLER:
				ptype = "SAMPLER";
				break;
			default:
				ptype = "BAD plugin";
				break;
			}
			printf("======= %s %s:\n", ptype, b);
			const char *u = pi->plugin->usage(pi->plugin);
			printf("%s\n", u);
			printf("=========================\n");
			rc = ldmsd_term_plugin(b);
			if (rc == ENOENT) {
				fprintf(stderr, "plugin '%s' not found\n", b);
			} else if (rc == EINVAL) {
				fprintf(stderr, "The specified plugin '%s' has "
					"active users and cannot be "
					"terminated.\n", b);
			} else if (rc) {
				fprintf(stderr, "Failed to terminate "
						"the plugin '%s'\n", b);
			}
 next:
			free(tmp);
		}

	}


 out2:
	globfree(&pglob);
	free(pat);
 out:
	return rc;
}

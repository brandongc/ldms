/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2025 Open Grid Computing, Inc. All rights reserved.
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
 * \file perfevent3.c
 * \brief perfevent3
 *
 * Reads perf counters according to config
 */

#define _GNU_SOURCE

#include "config.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <dirent.h>
#include <regex.h>

#include <linux/limits.h>

#include <sys/errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h> /* for uname */

#include <linux/perf_event.h>    /* Definition of PERF_* constants */
#include <linux/hw_breakpoint.h> /* Definition of HW_* constants */
#include <sys/syscall.h>         /* Definition of SYS_* constants */

#include <ftw.h>
#include <dlfcn.h>


#include "ovis_json/ovis_json.h"

#include "ldms.h"
#include "ldmsd.h"
#include "ldmsd_plug_api.h"
#include "../sampler_base.h"

#define _LOG(p, lvl, fmt, ...) ovis_log((p)->log, lvl, fmt, ## __VA_ARGS__ )

#ifdef PERFEVENT3_DEBUG
/* abort on error for debugging */
#define _ERROR(p, fmt, ...) do {\
		_LOG(p, OVIS_LERROR, fmt, ## __VA_ARGS__ ); \
		abort(); \
	} while(0)
#else
#define _ERROR(p, fmt, ...) _LOG(p, OVIS_LERROR, fmt, ## __VA_ARGS__ )
#endif

#define _WARN(p, fmt, ...) _LOG(p, OVIS_LWARN, fmt, ## __VA_ARGS__ )
#define _INFO(p, fmt, ...) _LOG(p, OVIS_LINFO, fmt, ## __VA_ARGS__ )
#define _DEBUG(p,fmt, ...) _LOG(p, OVIS_LDEBUG, fmt, ## __VA_ARGS__ )

#define likely(cond) __builtin_expect(!!(cond), 1)
#define unlikely(cond) __builtin_expect(!!(cond), 0)

#define SAMP "perfevent3"
#define MAX_CPU 1024
#define PMU_EVENT_NAME_SZ 256
#define PMU_EVENT_VALUE_SZ 512

enum template_type_e {
	TEMPLATE_UNSET = 0,
	TEMPLATE_CPU,
	TEMPLATE_CACHE,
	TEMPLATE_SOCKET,
	TEMPLATE_NODE,
	TEMPLATE_PMU,
};

struct instance_s {
	uint32_t index;
	enum template_type_e type;
	char name[128];
	char pmu[128];
	int representative_cpu;
	int socket_id;
	int die_id;
	int node_id;
	int cache_level;
	int cache_id;
	uint8_t cpu_mask[MAX_CPU];
};

struct event_rec_s {
	TAILQ_ENTRY(event_rec_s) entry;
	struct pmu_event_s *pmu_event;
	json_entity_t conf_event;
	char name[PMU_EVENT_NAME_SZ];
	char event_spec[PMU_EVENT_VALUE_SZ];
	char pmu[128];
	pid_t pid;
	int is_scaled;
	ldms_mval_t rec_mval;
	ldms_mval_t values_mval;
};

struct counter_s {
	TAILQ_ENTRY(counter_s) entry;
	struct pmu_event_s *pmu_event;
	json_entity_t conf_event; /* conf_event, from config file */
	struct event_rec_s *event_rec;
	struct perf_event_attr attr;
	pid_t pid;
	int   cpu;
	int   instance_idx;
	int   fd; /* from perf_event_open() */
	ldms_mval_t mval; /* mval to event_rec->values[instance_idx] */
};

struct dev_s {
	TAILQ_ENTRY(counter_s) entry;
	TAILQ_HEAD(, counter_s) counter_tq;
};

/* perf instance ctxt */
struct perf_s {
	base_data_t base;
	ldmsd_plug_handle_t plug;
	ovis_log_t log;

	ldms_set_t set;

	int counter_recdef_midx; /* to counter recdef */
	int counters_midx; /* idx to list of counters */
	ldms_mval_t counters_mval; /* list of counters */

	int scounter_recdef_midx; /* to scaled counter recdef */
	int scounters_midx; /* idx list of scaled counters */
	ldms_mval_t scounters_mval; /* list of scaled counters */

	int instance_recdef_midx;
	int instances_midx;
	ldms_mval_t instances_mval;
	struct instance_s *instances;
	int instance_count;
	enum template_type_e template_type;

	int n_cpu; /* number of CPUs */

	char cpu_model[256];

	int n_counters; /* count specific event-cpu-pid */
	int n_events;  /* events with unscaled counter */
	int n_sevents; /* events with scaled counter */
	TAILQ_HEAD(, counter_s) counters;
	TAILQ_HEAD(, event_rec_s) events;
	json_doc_t perfdb; /* object from perfdb file */
	json_doc_t config; /* config object from config file */

	json_entity_t model; /* the matching model (micro arch); part of perfdb */
	json_entity_t model_events; /* model['events'] */

	struct rbt pmu_rbt; /* contains pmu_s */

	struct rbt pmu_event_idx_rbt; /* contains pmu_event_idx_s */

	struct rbt builtin_events_rbt; /* built-in events (hw & cache) */

	struct utsname uname;

	int (*set_cpu_model)(struct perf_s *p);

	int (*cpu_match)(struct perf_s *p, const char *family_model);
};

#define PMU_FORMAT_NAME_SZ 128

struct pmu_format_s {
	struct rbn rbn; /* in pmu_s.format_rbt */
	char   name[PMU_FORMAT_NAME_SZ];
	int config;    /* 0, 1, 2, or 3 */
	uint64_t mask; /* bit masks     */
};

/* format-value pair */
struct pmu_term_s {
	struct pmu_format_s *format;
	int64_t value;
};

struct pmu_event_s {
	struct rbn rbn; /* in pmu_s.events_rbt */
	double scale; /* 0.0 means no scale */
	struct perf_event_attr attr;
	char   name[PMU_EVENT_NAME_SZ];
	char   unit[PMU_EVENT_NAME_SZ];
	char   event_value[PMU_EVENT_VALUE_SZ]; /* for debugging */
	int    n_terms;
	struct pmu_s *pmu; /* for convenient; built-ins (e.g. hw and cache) may not have PMU struct */
	struct pmu_term_s terms[OVIS_FLEX];
};

/* an index to pmu_event_s */
struct pmu_event_idx_s {
	struct rbn rbn;
	struct pmu_event_s *pmu_event;
	int name_len; /* strlen(name) */
	char name[];
};

/* contain pmu info */
struct pmu_s {
	struct rbn rbn; /* in perf_s.pmu_rbt */
	char   name[256];
	struct rbt format_rbt; /* "event_source/devices/<PMU>/format/"*/
	struct rbt events_rbt; /* "event_source/devices/<PMU>/events/"*/
	uint32_t type;
	uint8_t cpu_mask[1024]; /* should be enough ... */
};

static const char *usage(ldmsd_plug_handle_t handle)
{
	(void)handle;
	return "config name={INSTANCE} conf={EVENT_CONFIG_JSON_FILE} [perfdb={PERF_DB_FILE}]\n";
}

static json_doc_t default_perfdb; /* perfdb from `perf list --details hw cache pmu` */

static int perf_event_open(struct perf_event_attr *attr,
			   pid_t pid, int cpu, int group_fd,
			   unsigned long flags);

static void perf_close(struct perf_s *p);
static struct pmu_s *pmu_find(struct perf_s *p, const char *name);

static int rbt_str_cmp(void *tree_key, const void *key)
{
	return strcmp(tree_key, key);
}

static inline struct rbn * RBT_DEL(struct rbt *rbt, struct rbn *rbn)
{
	if (rbn)
		rbt_del(rbt, rbn);
	return rbn;
}
#define RBT_FOREACH_DEL(rbn, rbt) for ((rbn) = rbt_min(rbt); RBT_DEL(rbt, rbn) ; (rbn) = rbt_min(rbt))

void pmu_free(struct pmu_s *pmu);
static const char *json_value_find_cstr(struct perf_s *p, json_entity_t e,
					const char *attr_name);

static const bool is_sep[] = {
	['\n'] = 1,
	[' '] = 1,
	[','] = 1,
	[255] = 0,
};

static const bool is_digit[] = {
	['0'] = 1,
	['1'] = 1,
	['2'] = 1,
	['3'] = 1,
	['4'] = 1,
	['5'] = 1,
	['6'] = 1,
	['7'] = 1,
	['8'] = 1,
	['9'] = 1,
	[255] = 0,
};

static void perf_config_data_free(struct perf_s *p)
{
	struct counter_s *c;
	struct event_rec_s *er;

	perf_close(p);
	while ((c = TAILQ_FIRST(&p->counters))) {
		TAILQ_REMOVE(&p->counters, c, entry);
		free(c);
	}

	while ((er = TAILQ_FIRST(&p->events))) {
		TAILQ_REMOVE(&p->events, er, entry);
		free(er);
	}

	free(p->instances);
	p->instances = NULL;
	p->instance_count = 0;
	p->template_type = TEMPLATE_UNSET;
	p->n_counters = 0;
	p->n_events = 0;
	p->n_sevents = 0;
	p->instance_recdef_midx = -1;
	p->instances_midx = -1;
	p->instances_mval = NULL;
	p->counter_recdef_midx = -1;
	p->counters_midx = -1;
	p->counters_mval = NULL;
	p->scounter_recdef_midx = -1;
	p->scounters_midx = -1;
	p->scounters_mval = NULL;
	p->set = NULL;
}

static void perf_free(struct perf_s *p)
{
	struct rbn *rbn;
	struct pmu_s *pmu;
	struct pmu_event_s *event;

	if (!p)
		return;

	perf_config_data_free(p);

	if (p->base) {
		base_set_delete(p->base);
		base_del(p->base);
	}

	/* clearing pmu_event_idx tree */
	RBT_FOREACH_DEL(rbn, &p->pmu_event_idx_rbt) {
		free(rbn);
	}

	/* clearing pmu's */
	RBT_FOREACH_DEL(rbn, &p->pmu_rbt) {
		pmu = container_of(rbn, struct pmu_s, rbn);
		pmu_free(pmu);
	}

	/* clearing builtin events */
	RBT_FOREACH_DEL(rbn, &p->builtin_events_rbt) {
		event = container_of(rbn, struct pmu_event_s, rbn);
		free(event);
	}

	/* p->model and p->model_events are part of p->perfdb */
	if (p->perfdb && p->perfdb != default_perfdb)
		json_doc_free(p->perfdb);
	if (p->config)
		json_doc_free(p->config);
	free(p);
}

static void destructor(ldmsd_plug_handle_t handle)
{
	struct perf_s *p = ldmsd_plug_ctxt_get(handle);
	perf_close(p);
	perf_free(p);
}

static int xread_buf(struct perf_s *p, const char *path, char *buf, int bufsz)
{
	/*
	 * Similar to `snprintf()`, this function returns the number of bytes
	 * read into `buf` (excluding terminating '\0'). If bufsz cannot hold
	 * the file content, this function fills `buf` to its limit (bufsz-1 and
	 * terminates with '\0') and the file size is returned.
	 */
	int fd = -1;
	off_t off;
	ssize_t sz, rsz, fsz;
	int ret;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		ret = -1;
		_ERROR(p, "failed to open '%s', errno: %d\n", path, errno);
		goto out;
	}

	off = lseek(fd, 0, SEEK_END);
	if (off < 0) {
		ret = -1;
		_ERROR(p, "lseek() failed, errno: %d\n", errno);
		goto out;
	}

	fsz = off + 1;
	sz = fsz<bufsz?fsz:(bufsz-1);

	off = lseek(fd, 0, SEEK_SET);
	if (off < 0) {
		ret = -1;
		_ERROR(p, "lseek() failed, errno: %d\n", errno);
		goto out;
	}

	off = 0;
	while (sz) {
		rsz = read(fd, buf + off, sz);
		if (rsz < 0) {
			_ERROR(p, "read() error: %d\n", errno);
			ret = -1;
			goto out;
		}
		if (rsz == 0) {
			/* EOF */
			break;
		}
		off += rsz;
		sz -= rsz;
	}
	buf[off] = 0;
	if (rsz) {
		/* EOF has not reached */
		ret = fsz;
	} else {
		/* reached EOF */
		ret = off;
	}

 out:
	if (fd >= 0)
		close(fd);
	return ret;
}

static char *xread_str(struct perf_s *p, const char *path, int *slen)
{
	int fd = -1;
	off_t off;
	ssize_t sz, rsz;
	char *buff = NULL;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		_ERROR(p, "failed to open '%s', errno: %d\n", path, errno);
		goto out;
	}

	off = lseek(fd, 0, SEEK_END);
	if (off < 0) {
		_ERROR(p, "lseek() failed, errno: %d\n", errno);
		goto out;
	}

	sz = off + 1;
	buff = malloc(sz+1);
	if (!buff) {
		_ERROR(p, "Out of memory\n");
		goto out;
	}

	off = lseek(fd, 0, SEEK_SET);
	if (off < 0) {
		_ERROR(p, "lseek() failed, errno: %d\n", errno);
		goto out;
	}

	off = 0;
	while (sz) {
		rsz = read(fd, buff + off, sz);
		if (rsz < 0) {
			_ERROR(p, "read() error: %d\n", errno);
			goto out;
		}
		if (rsz == 0)
			break;
		off += rsz;
		sz -= rsz;
	}
	buff[off] = 0;
	if (slen)
		*slen = off; /* strlen */

 out:
	if (fd >= 0)
		close(fd);
	return buff;
}

static int xread_long(struct perf_s *p, const char *path, long *out)
{
	/* returns 0 on success, errno on error */

	char buf[256]; /* plenty for a number */
	int len;

	if (!out)
		return EINVAL;

	len = xread_buf(p, path, buf, sizeof(buf));

	if (len < 0)
		return errno; /* already logged by xread_buf() */

	if (unlikely((size_t)len >= sizeof(buf)))
		return EINVAL; /* already logged by xread_buf() */

	errno = 0;
	*out = strtol(buf, NULL, 0); /* errno is set on error */
	if (errno) {
		_ERROR(p, "Bad (long) number: %s\n", buf);
		return errno;
	}

	return 0;
}

static int xread_double(struct perf_s *p, const char *path, double *out)
{
	/* returns 0 on success, errno on error */

	char buf[256]; /* plenty for a number */
	int len;

	if (!out)
		return EINVAL;

	len = xread_buf(p, path, buf, sizeof(buf));

	if (len < 0)
		return errno; /* already logged by xread_buf() */

	if (unlikely((size_t)len >= sizeof(buf)))
		return EINVAL; /* already logged by xread_buf() */

	errno = 0;
	*out = strtod(buf, NULL); /* errno is set on error */
	if (errno) {
		_ERROR(p, "Bad (double) number: %s\n", buf);
		return errno;
	}

	return 0;
}

/* need perf_s p for logging */
static json_doc_t xread_json(struct perf_s *p, const char *path)
{
	json_doc_t jdoc;
	char *buff = NULL;
	int rc;
	int slen;

	buff = xread_str(p, path, &slen);
	if (!buff)
		return NULL;

	rc = json_parse_buffer(buff, slen, &jdoc);
	if (rc) {
		_ERROR(p, "json parse error: %s\n", json_doc_errstr(jdoc));
		json_doc_free(jdoc);
		errno = EINVAL;
		return NULL;
	}
	free(buff);
	return jdoc;
}

static int load_perfdb(struct perf_s *p, const char *perfdb)
{
	struct utsname u;
	const char *src_ver;
	int rc, len;

	if (p->perfdb)
		return EEXIST;
	_INFO(p, "Loading perfdb: %s\n", perfdb);
	p->perfdb = xread_json(p, perfdb);
	if (!p->perfdb)
		return EINVAL;

	/* Check Linux version and give a warning if a mismatch is detected. */
	rc = uname(&u);
	if (rc) {
		_WARN(p, "uname() error: %d\n", errno);
		goto out;
	}

	src_ver = json_value_find_cstr(p, json_doc_root(p->perfdb),
				       "linux_src_ver");
	if (!src_ver) {
		_WARN(p, "'linux_src_ver' attribute not found in perfdb.\n");
		goto out;
	}

	len = strlen(src_ver);
	if (0 != strncmp(src_ver, u.release, len) || is_digit[(uint8_t)u.release[len]]) {
		_WARN(p, "perfdb is generated from Linux version '%s'"
			 " while the running kernel version is '%s'\n",
			 src_ver, u.release);
		goto out;
	}

 out:
	return 0;
}

struct range_s {
	/* inclusive */
	int64_t min;
	int64_t max;
	TAILQ_ENTRY(range_s) entry;
};

struct ranges_s {
	TAILQ_HEAD(, range_s) ranges;
	struct range_s *curr_range;
	int64_t curr_num;
};

void ranges_free(struct ranges_s *rs)
{
	struct range_s *r;
	if (!rs)
		return;
	while ((r = TAILQ_FIRST(&rs->ranges))) {
		TAILQ_REMOVE(&rs->ranges, r, entry);
		free(r);
	}
	free(rs);
}

struct ranges_s *ranges_new(struct perf_s *p, const char *str)
{
	/* str example: "10-15,20,25-30" */

	struct ranges_s *rs;
	const char *s = str;
	struct range_s *r;
	int n, lena, lenb;

	rs = malloc(sizeof(*rs));
	if (!rs)
		return NULL;

	TAILQ_INIT(&rs->ranges);
	rs->curr_range = NULL;

	while (*s) {
		if (is_sep[(unsigned char)*s]) {
			s += 1;
			continue;
		}
		r = malloc(sizeof(*r));
		if (!r)
			goto err;
		n = sscanf(s, "%ld%n-%ld%n", &r->min, &lena, &r->max, &lenb);
		if (n == 1) {
			s += lena;
			r->max = r->min;
		} else if (n == 2) {
			s += lenb;
		} else {
			_ERROR(p, "bad range: %s", s);
			goto err;
		}
		TAILQ_INSERT_TAIL(&rs->ranges, r, entry);
	}

	return rs;

 err:
	ranges_free(rs);
	return NULL;
}

int ranges_first(struct ranges_s *rs, int64_t *ret)
{
	rs->curr_range = TAILQ_FIRST(&rs->ranges);
	if (!rs->curr_range)
		return ENOENT;
	rs->curr_num = rs->curr_range->min;
	if (ret)
		*ret = rs->curr_num;
	return 0;
}

void ranges_curr(struct ranges_s *rs, int64_t *ret)
{
	*ret = rs->curr_num;
}

int ranges_next(struct ranges_s *rs, int64_t *ret)
{
	if (!rs->curr_range)
		return ranges_first(rs, ret);
	rs->curr_num++;
	if (rs->curr_num <= rs->curr_range->max) {
		goto out;
	}
	rs->curr_range = TAILQ_NEXT(rs->curr_range, entry);
	if (rs->curr_range) {
		rs->curr_num = rs->curr_range->min;
		goto out;
	}
	return ENOENT;

 out:
	if (ret)
		*ret = rs->curr_num;
	return 0;
}

static const char *template_type_name(enum template_type_e t)
{
	switch (t) {
	case TEMPLATE_CPU:
		return "cpu";
	case TEMPLATE_CACHE:
		return "cache";
	case TEMPLATE_SOCKET:
		return "socket";
	case TEMPLATE_NODE:
		return "node";
	case TEMPLATE_PMU:
		return "pmu";
	default:
		return "unset";
	}
}

static enum template_type_e template_type_parse(const char *s)
{
	if (!s)
		return TEMPLATE_UNSET;
	if (0 == strcmp(s, "cpu"))
		return TEMPLATE_CPU;
	if (0 == strcmp(s, "cache"))
		return TEMPLATE_CACHE;
	if (0 == strcmp(s, "socket"))
		return TEMPLATE_SOCKET;
	if (0 == strcmp(s, "node"))
		return TEMPLATE_NODE;
	if (0 == strcmp(s, "pmu"))
		return TEMPLATE_PMU;
	return TEMPLATE_UNSET;
}

static void cpu_mask_zero(uint8_t *mask)
{
	bzero(mask, MAX_CPU);
}

static void cpu_mask_set_cpu(uint8_t *mask, int cpu)
{
	if (cpu >= 0 && cpu < MAX_CPU)
		mask[cpu] = 1;
}

static void cpu_mask_copy(uint8_t *dst, const uint8_t *src)
{
	memcpy(dst, src, MAX_CPU);
}

static void cpu_mask_all(struct perf_s *p, uint8_t *mask)
{
	int i;
	cpu_mask_zero(mask);
	for (i = 0; i < p->n_cpu && i < MAX_CPU; i++)
		mask[i] = 1;
}

static int cpu_mask_first(struct perf_s *p, const uint8_t *mask)
{
	int i;
	for (i = 0; i < p->n_cpu && i < MAX_CPU; i++) {
		if (mask[i])
			return i;
	}
	return -1;
}

static int cpu_mask_first_intersection(struct perf_s *p, const uint8_t *a, const uint8_t *b)
{
	int i;
	for (i = 0; i < p->n_cpu && i < MAX_CPU; i++) {
		if (a[i] && b[i])
			return i;
	}
	return -1;
}

static int cpu_mask_parse(struct perf_s *p, const char *str, uint8_t *mask)
{
	struct ranges_s *rs;
	int64_t cpu;
	int rc;

	cpu_mask_zero(mask);
	rs = ranges_new(p, str);
	if (!rs)
		return errno;
	rc = ranges_first(rs, &cpu);
	while (0 == rc) {
		if (cpu >= 0 && cpu < p->n_cpu && cpu < MAX_CPU)
			mask[cpu] = 1;
		else
			_WARN(p, "Ignoring CPU %ld outside [0-%d].\n", cpu, p->n_cpu-1);
		rc = ranges_next(rs, &cpu);
	}
	ranges_free(rs);
	return 0;
}

static int cpu_mask_from_json(struct perf_s *p, json_entity_t parent,
			      const char *attr, uint8_t *mask)
{
	json_entity_t e;
	enum json_value_e t;
	char buf[64];

	e = json_value_find(parent, attr);
	if (!e)
		return ENOENT;
	t = json_entity_type(e);
	if (t == JSON_STRING_VALUE)
		return cpu_mask_parse(p, json_value_cstr(e), mask);
	if (t == JSON_INT_VALUE) {
		snprintf(buf, sizeof(buf), "%ld", json_value_int(e));
		return cpu_mask_parse(p, buf, mask);
	}
	_ERROR(p, "'%s' must be an integer or CPU range string.\n", attr);
	return EINVAL;
}

static int read_sysfs_int_optional(struct perf_s *p, const char *path, int dflt)
{
	int fd;
	char buf[64];
	ssize_t sz;
	long out;

	(void)p;
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return dflt;
	sz = read(fd, buf, sizeof(buf)-1);
	close(fd);
	if (sz <= 0)
		return dflt;
	buf[sz] = '\0';
	errno = 0;
	out = strtol(buf, NULL, 0);
	if (errno)
		return dflt;
	return out;
}

static int cpu_socket_id(struct perf_s *p, int cpu)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu);
	return read_sysfs_int_optional(p, path, -1);
}

static int cpu_die_id(struct perf_s *p, int cpu)
{
	char path[PATH_MAX];
	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/topology/die_id", cpu);
	return read_sysfs_int_optional(p, path, -1);
}

static int cpu_node_id(struct perf_s *p, int cpu)
{
	DIR *d;
	struct dirent *dent;
	char path[PATH_MAX];
	char buf[4096];
	struct ranges_s *rs;
	int64_t v;
	int rc, node_id = -1;

	d = opendir("/sys/devices/system/node");
	if (!d)
		return -1;
	while ((dent = readdir(d))) {
		if (1 != sscanf(dent->d_name, "node%d", &node_id))
			continue;
		snprintf(path, sizeof(path), "/sys/devices/system/node/%s/cpulist",
			 dent->d_name);
		rc = xread_buf(p, path, buf, sizeof(buf));
		if (rc < 0 || (size_t)rc >= sizeof(buf))
			continue;
		rs = ranges_new(p, buf);
		if (!rs)
			continue;
		rc = ranges_first(rs, &v);
		while (0 == rc) {
			if (v == cpu) {
				ranges_free(rs);
				closedir(d);
				return node_id;
			}
			rc = ranges_next(rs, &v);
		}
		ranges_free(rs);
	}
	closedir(d);
	return -1;
}

static int cpu_cache_info(struct perf_s *p, int cpu, int level, int *cache_id,
			  uint8_t *shared_mask)
{
	DIR *d;
	struct dirent *dent;
	char dir[PATH_MAX];
	char path[PATH_MAX];
	char type[64];
	char shared[4096];
	int lvl;
	int rc = ENOENT;

	snprintf(dir, sizeof(dir), "/sys/devices/system/cpu/cpu%d/cache", cpu);
	d = opendir(dir);
	if (!d)
		return ENOENT;
	while ((dent = readdir(d))) {
		if (0 != strncmp(dent->d_name, "index", 5))
			continue;
		snprintf(path, sizeof(path), "%s/%s/level", dir, dent->d_name);
		lvl = read_sysfs_int_optional(p, path, -1);
		if (lvl != level)
			continue;
		snprintf(path, sizeof(path), "%s/%s/type", dir, dent->d_name);
		if (xread_buf(p, path, type, sizeof(type)) < 0)
			continue;
		if (0 != strncmp(type, "Unified", 7))
			continue;
		snprintf(path, sizeof(path), "%s/%s/id", dir, dent->d_name);
		*cache_id = read_sysfs_int_optional(p, path, -1);
		snprintf(path, sizeof(path), "%s/%s/shared_cpu_list", dir, dent->d_name);
		if (xread_buf(p, path, shared, sizeof(shared)) < 0)
			continue;
		rc = cpu_mask_parse(p, shared, shared_mask);
		if (rc)
			continue;
		rc = 0;
		break;
	}
	closedir(d);
	return rc;
}

static int instance_add(struct perf_s *p, struct instance_s *inst)
{
	struct instance_s *tmp;

	tmp = realloc(p->instances, sizeof(*p->instances) * (p->instance_count + 1));
	if (!tmp)
		return ENOMEM;
	p->instances = tmp;
	inst->index = p->instance_count;
	p->instances[p->instance_count++] = *inst;
	return 0;
}

static struct instance_s *instance_find_cache(struct perf_s *p, int cache_level, int cache_id)
{
	int i;
	for (i = 0; i < p->instance_count; i++) {
		if (p->instances[i].cache_level == cache_level &&
		    p->instances[i].cache_id == cache_id)
			return &p->instances[i];
	}
	return NULL;
}

static struct instance_s *instance_find_int_id(struct perf_s *p, enum template_type_e type, int id)
{
	int i;
	for (i = 0; i < p->instance_count; i++) {
		if (type == TEMPLATE_SOCKET && p->instances[i].socket_id == id)
			return &p->instances[i];
		if (type == TEMPLATE_NODE && p->instances[i].node_id == id)
			return &p->instances[i];
	}
	return NULL;
}

static int template_build_cpu(struct perf_s *p, uint8_t *selected)
{
	int cpu, rc;
	struct instance_s inst;

	for (cpu = 0; cpu < p->n_cpu && cpu < MAX_CPU; cpu++) {
		if (!selected[cpu])
			continue;
		bzero(&inst, sizeof(inst));
		inst.type = TEMPLATE_CPU;
		inst.representative_cpu = cpu;
		inst.socket_id = cpu_socket_id(p, cpu);
		inst.die_id = cpu_die_id(p, cpu);
		inst.node_id = cpu_node_id(p, cpu);
		inst.cache_level = -1;
		inst.cache_id = -1;
		snprintf(inst.name, sizeof(inst.name), "cpu%d", cpu);
		snprintf(inst.pmu, sizeof(inst.pmu), "cpu");
		cpu_mask_set_cpu(inst.cpu_mask, cpu);
		rc = instance_add(p, &inst);
		if (rc)
			return rc;
	}
	return p->instance_count ? 0 : ENOENT;
}

static int template_build_cache(struct perf_s *p, uint8_t *selected, int level)
{
	int cpu, cache_id, rc;
	uint8_t shared_mask[MAX_CPU];
	struct instance_s inst;

	if (level <= 0)
		return EINVAL;
	for (cpu = 0; cpu < p->n_cpu && cpu < MAX_CPU; cpu++) {
		if (!selected[cpu])
			continue;
		cpu_mask_zero(shared_mask);
		rc = cpu_cache_info(p, cpu, level, &cache_id, shared_mask);
		if (rc)
			continue;
		if (instance_find_cache(p, level, cache_id))
			continue;
		bzero(&inst, sizeof(inst));
		inst.type = TEMPLATE_CACHE;
		inst.representative_cpu = cpu_mask_first_intersection(p, selected, shared_mask);
		if (inst.representative_cpu < 0)
			inst.representative_cpu = cpu;
		inst.socket_id = cpu_socket_id(p, inst.representative_cpu);
		inst.die_id = cpu_die_id(p, inst.representative_cpu);
		inst.node_id = cpu_node_id(p, inst.representative_cpu);
		inst.cache_level = level;
		inst.cache_id = cache_id;
		snprintf(inst.name, sizeof(inst.name), "L%d:%d", level, cache_id);
		snprintf(inst.pmu, sizeof(inst.pmu), "cache");
		cpu_mask_copy(inst.cpu_mask, shared_mask);
		rc = instance_add(p, &inst);
		if (rc)
			return rc;
	}
	return p->instance_count ? 0 : ENOENT;
}

static int template_build_socket_or_node(struct perf_s *p, uint8_t *selected,
					 enum template_type_e type)
{
	int cpu, id, rc;
	struct instance_s *old;
	struct instance_s inst;

	for (cpu = 0; cpu < p->n_cpu && cpu < MAX_CPU; cpu++) {
		if (!selected[cpu])
			continue;
		id = (type == TEMPLATE_SOCKET) ? cpu_socket_id(p, cpu) : cpu_node_id(p, cpu);
		if (id < 0)
			continue;
		old = instance_find_int_id(p, type, id);
		if (old) {
			cpu_mask_set_cpu(old->cpu_mask, cpu);
			continue;
		}
		bzero(&inst, sizeof(inst));
		inst.type = type;
		inst.representative_cpu = cpu;
		inst.socket_id = cpu_socket_id(p, cpu);
		inst.die_id = cpu_die_id(p, cpu);
		inst.node_id = cpu_node_id(p, cpu);
		inst.cache_level = -1;
		inst.cache_id = -1;
		snprintf(inst.name, sizeof(inst.name), "%s%d", template_type_name(type), id);
		snprintf(inst.pmu, sizeof(inst.pmu), "%s", template_type_name(type));
		cpu_mask_set_cpu(inst.cpu_mask, cpu);
		rc = instance_add(p, &inst);
		if (rc)
			return rc;
	}
	return p->instance_count ? 0 : ENOENT;
}

static int template_build_pmu(struct perf_s *p, json_entity_t template)
{
	json_entity_t e_pmus, e;
	enum json_value_e t;
	const char *pmu_name;
	struct pmu_s *pmu;
	struct instance_s inst;
	int cpu, rc;

	e_pmus = json_value_find(template, "pmus");
	if (!e_pmus || JSON_LIST_VALUE != json_entity_type(e_pmus)) {
		_ERROR(p, "template.type='pmu' requires string list 'pmus'.\n");
		return EINVAL;
	}
	for (e = json_item_first(e_pmus); e; e = json_item_next(e)) {
		t = json_entity_type(e);
		if (t != JSON_STRING_VALUE) {
			_ERROR(p, "template.pmus entries must be strings.\n");
			return EINVAL;
		}
		pmu_name = json_value_cstr(e);
		pmu = pmu_find(p, pmu_name);
		if (!pmu)
			return errno;
		cpu = cpu_mask_first(p, pmu->cpu_mask);
		if (cpu < 0) {
			_ERROR(p, "PMU '%s' has no usable CPU in cpumask.\n", pmu_name);
			return ENOENT;
		}
		bzero(&inst, sizeof(inst));
		inst.type = TEMPLATE_PMU;
		inst.representative_cpu = cpu;
		inst.socket_id = cpu_socket_id(p, cpu);
		inst.die_id = cpu_die_id(p, cpu);
		inst.node_id = cpu_node_id(p, cpu);
		inst.cache_level = -1;
		inst.cache_id = -1;
		snprintf(inst.name, sizeof(inst.name), "%s", pmu_name);
		snprintf(inst.pmu, sizeof(inst.pmu), "%s", pmu_name);
		cpu_mask_copy(inst.cpu_mask, pmu->cpu_mask);
		rc = instance_add(p, &inst);
		if (rc)
			return rc;
	}
	return p->instance_count ? 0 : ENOENT;
}

static int template_build(struct perf_s *p, json_entity_t config)
{
	json_entity_t e_template, e_type, e_level;
	enum template_type_e type;
	uint8_t selected[MAX_CPU];
	const char *type_str;
	int level = -1;
	int rc;

	e_template = json_value_find(config, "template");
	if (!e_template || JSON_DICT_VALUE != json_entity_type(e_template)) {
		_ERROR(p, "'template' object is required for %s.\n", SAMP);
		return EINVAL;
	}
	e_type = json_value_find(e_template, "type");
	if (!e_type || JSON_STRING_VALUE != json_entity_type(e_type)) {
		_ERROR(p, "template.type string is required.\n");
		return EINVAL;
	}
	type_str = json_value_cstr(e_type);
	type = template_type_parse(type_str);
	if (type == TEMPLATE_UNSET) {
		_ERROR(p, "Unsupported template.type '%s'.\n", type_str);
		return EINVAL;
	}
	p->template_type = type;

	cpu_mask_all(p, selected);
	rc = cpu_mask_from_json(p, e_template, "cpus", selected);
	if (rc == ENOENT)
		rc = cpu_mask_from_json(p, config, "cpu", selected);
	if (rc == ENOENT)
		rc = 0;
	if (rc)
		return rc;

	switch (type) {
	case TEMPLATE_CPU:
		rc = template_build_cpu(p, selected);
		break;
	case TEMPLATE_CACHE:
		e_level = json_value_find(e_template, "level");
		if (!e_level || JSON_INT_VALUE != json_entity_type(e_level)) {
			_ERROR(p, "template.type='cache' requires integer 'level'.\n");
			return EINVAL;
		}
		level = json_value_int(e_level);
		rc = template_build_cache(p, selected, level);
		break;
	case TEMPLATE_SOCKET:
	case TEMPLATE_NODE:
		rc = template_build_socket_or_node(p, selected, type);
		break;
	case TEMPLATE_PMU:
		rc = template_build_pmu(p, e_template);
		break;
	default:
		rc = EINVAL;
	}
	if (rc)
		_ERROR(p, "Failed to build '%s' instance template: %s\n",
		       template_type_name(type), strerror(rc));
	else
		_INFO(p, "Built '%s' instance template with %d instances.\n",
		      template_type_name(type), p->instance_count);
	return rc;
}

static int pmu_event_index(struct perf_s *p, struct pmu_event_s *pmu_event, const char *key)
{
	int len = strlen(key);
	struct pmu_event_idx_s *idx;

	idx = malloc(sizeof(*idx) + len + 1);
	if (unlikely(!idx)) {
		_ERROR(p, "pmu_event_index(): Out of memory\n");
		return ENOMEM;
	}
	memcpy(idx->name, key, len+1);
	idx->name_len = len;
	idx->pmu_event = pmu_event;
	rbn_init(&idx->rbn, idx->name);
	rbt_ins(&p->pmu_event_idx_rbt, &idx->rbn);
	return 0;
}

void pmu_free(struct pmu_s *pmu)
{
	struct rbn *rbn;
	struct pmu_event_s *pmu_event;
	struct pmu_format_s *pmu_format;

	while ((rbn = rbt_min(&pmu->events_rbt))) {
		rbt_del(&pmu->events_rbt, rbn);
		pmu_event = container_of(rbn, struct pmu_event_s, rbn);
		free(pmu_event);
	}

	while ((rbn = rbt_min(&pmu->format_rbt))) {
		rbt_del(&pmu->format_rbt, rbn);
		pmu_format = container_of(rbn, struct pmu_format_s, rbn);
		free(pmu_format);
	}

	free(pmu);
}

static int pmu_format_parse(struct perf_s *p, struct pmu_format_s *pmu_format,
			    const char *format_str, size_t len)
{
	/* format_str = "config[123]?:<RANGES>" */
	const char *s = format_str;
	int n;
	int off0, off1;
	int64_t num;
	struct ranges_s *rs;

	(void)len;
	pmu_format->config = 0;
	pmu_format->mask = 0;
	off0 = off1 = 0;

	n = sscanf(s, "config%n%d%n", &off0, &pmu_format->config, &off1);
	if (unlikely(!off0))
		goto format_err;
	s += n?off1:off0;

	/* expecting ':' */
	if (*s != ':')
		goto format_err;
	s++;

	rs = ranges_new(p, s);
	if (!rs)
		return errno;
	while ((0 == ranges_next(rs, &num))) {
		if (num < 0 || num >= 64) {
			ranges_free(rs);
			_ERROR(p, "PMU format bit %" PRId64 " is outside [0-63].\n", num);
			return EINVAL;
		}
		pmu_format->mask |= (1ULL << num);
	}
	ranges_free(rs);
	return 0;

 format_err:
	_ERROR(p, "Expecting 'config[123]:...' but got: %s\n", format_str);
	return EINVAL;
}

static const char * get_default_core_name()
{
	/*
	 * From linux/tools/perf/util/pmu.c: is_pmu_core() and
	 * is_sysfs_pmu_core() functions, we can determine the PMU name of the
	 * default_core by:
	 * - "cpu" if "/sys/bus/event_source/devices/cpu" existed,
	 * - "cpum_cf" if "/sys/bus/event_source/devices/cpu" existed,
	 * - "<NAME>" if "/sys/bus/event_source/devices/<NAME>/cpus" existed
	 *   (e.g. ARM processors)
	 */

	static const char *name = NULL;
	static char buf[NAME_MAX+1];
	if (name)
		return name;

	struct stat st;
	char path[PATH_MAX];
	int rc;
	struct dirent *dent;
	DIR *d;

	rc = stat("/sys/bus/event_source/devices/cpu", &st);
	if (0 == rc) {
		name = "cpu";
		goto out;
	}

	rc = stat("/sys/bus/event_source/devices/cpum_cf", &st);
	if (0 == rc) {
		name = "cpum_cf";
		goto out;
	}

	d = opendir("/sys/bus/event_source/devices");
	if (!d)
		goto out;
	while ((dent = readdir(d))) {
		if (dent->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path),
			 "/sys/bus/event_source/devices/%s/cpus", dent->d_name);
		rc = stat(path, &st);
		if (0 == rc) {
			snprintf(buf, sizeof(buf), "%s", dent->d_name);
			name = buf;
			break;
		}
	}
	closedir(d);

 out:
	return name;
}

static struct pmu_s * pmu_find(struct perf_s *p, const char *name)
{
	struct pmu_s *pmu = NULL;
	int i, len;
	int rc;
	long num;
	char path[PATH_MAX];
	char buf[4096];
	DIR *d = NULL;
	struct dirent *dent;
	struct pmu_format_s *pmu_format;
	ssize_t off;
	struct stat st;

	struct rbn *rbn;

	rbn = rbt_find(&p->pmu_rbt, name);
	if (rbn) {
		pmu = container_of(rbn, struct pmu_s, rbn);
		goto out;
	}

	/* special PMU "default_core" handling */
	if (0 == strcmp(name, "default_core")) {
		name = get_default_core_name();
		if (!name) {
			_ERROR(p, "Cannot determine 'default_core'\n");
			goto err_0;
		}
	}

	pmu = calloc(1, sizeof(*pmu));
	if (!pmu)
		goto err_0;
	rbt_init(&pmu->events_rbt, rbt_str_cmp);
	rbt_init(&pmu->format_rbt, rbt_str_cmp);

	len = snprintf(pmu->name, sizeof(pmu->name), "%s", name);
	if (unlikely((size_t)len >= sizeof(pmu->name))) {
		_ERROR(p, "pmu '%s' name too long\n", name);
		errno = ENAMETOOLONG;
		goto err_0;
	}

	/* get type */
	snprintf(path, sizeof(path), "/sys/bus/event_source/devices/%s/type", name);
	rc = xread_long(p, path, &num);
	if (rc) {
		_ERROR(p, "%s: read error, errno: %d\n", path, rc);
		goto err_0;
	}
	pmu->type = num;

	/* load the formats */
	off = snprintf(path, sizeof(path),
			"/sys/bus/event_source/devices/%s/format", name);
	if (off < 0 || (size_t)off >= sizeof(path)) {
		errno = ENAMETOOLONG;
		_ERROR(p, "pmu '%s' format path too long\n", name);
		goto err_0;
	}
	d = opendir(path);
	if (!d) {
		_ERROR(p, "Cannot open PMU format directory '%s', errno: %d\n",
		       path, errno);
		goto err_0;
	}
	while ((dent = readdir(d))) {
		if (dent->d_name[0] == '.')
			continue;
		pmu_format = malloc(sizeof(*pmu_format));
		if (!pmu_format)
			goto err_0;
		snprintf(pmu_format->name, sizeof(pmu_format->name), "%.127s", dent->d_name);
		rbn_init(&pmu_format->rbn, pmu_format->name);
		snprintf(path+off, sizeof(path)-off, "/%s", pmu_format->name);
		len = xread_buf(p, path, buf, sizeof(buf));
		if (unlikely((size_t)len >= sizeof(buf)))
			goto err_0;
		rc = pmu_format_parse(p, pmu_format, buf, len);
		if (unlikely(rc))
			goto err_0;
		rbt_ins(&pmu->format_rbt, &pmu_format->rbn);
	}

	closedir(d);
	d = NULL;

	/* cpumask */
	bzero(pmu->cpu_mask, sizeof(pmu->cpu_mask));
	snprintf(path, sizeof(path), "/sys/bus/event_source/devices/%s/cpumask", name);
	rc = stat(path, &st);
	if (rc) {
		if (errno == ENOENT) {
			/* OK. no cpumask means this can be used for all cpus  */
			for (i = 0; i < p->n_cpu; i++) {
				pmu->cpu_mask[i] = 1;
			}

		} else {
			_ERROR(p, "'%s' stat error: %d\n", path, errno);
			goto err_0;
		}
	} else {
		len = xread_buf(p, path, buf, sizeof(buf));
		if (len < 0)
			goto err_0;
		if ((size_t)len >= sizeof(buf)) {
			_ERROR(p, "'%s' too big.\n", path);
			goto err_0;
		}
		struct ranges_s *rs;
		int64_t v;
		int rc;
		rs = ranges_new(p, buf);
		if (!rs) {
			goto err_0;
		}
		rc = ranges_first(rs, &v);
		while (0 == rc) {
			if (v >= p->n_cpu) {
				_ERROR(p, "Unexpected cpu %ld in %s.\n", v, path);
				ranges_free(rs);
				goto err_0;
			}
			pmu->cpu_mask[v] = 1;
			rc = ranges_next(rs, &v);
		}
		ranges_free(rs);
	}

	rbn_init(&pmu->rbn, pmu->name);
	rbt_ins(&p->pmu_rbt, &pmu->rbn);
 out:
	return pmu;

 err_0:
	if (d)
		closedir(d);
	if (pmu)
		pmu_free(pmu);
	return NULL;
}

static inline struct pmu_format_s *
pmu_format_find(struct perf_s *p, struct pmu_s *pmu, const char *name)
{
	struct rbn *rbn;
	struct pmu_format_s *f;
	rbn = rbt_find(&pmu->format_rbt, name);
	if (!rbn) {
		_ERROR(p, "Format '%s' not found.\n", name);
		return NULL;
	}
	f = container_of(rbn, struct pmu_format_s, rbn);
	return f;
}

/*
 * Align the unformatted `rvalue` according to `format_bits` and set it to `*lvalue`.
 */
static inline void pmu_format_value_set(uint64_t format_bits, unsigned long long *lvalue, uint64_t rvalue)
{
	uint64_t f, v;
	for (f = 1, v = 1; f; f <<= 1) {
		if (!(format_bits & f))
			continue;
		if (rvalue & v) {
			*lvalue |= f;
		} else {
			*lvalue &= (~f);
		}
		v <<= 1;
	}
}

/* ****************************** */
/* excerpt from linux/tools/perf/ */
/* ****************************** */

struct event_symbol {
	const char *symbol;
	const char *alias;
};

const struct event_symbol event_symbols_hw[PERF_COUNT_HW_MAX] = {
	[PERF_COUNT_HW_CPU_CYCLES] = {
		.symbol = "cpu-cycles",
		.alias  = "cycles",
	},
	[PERF_COUNT_HW_INSTRUCTIONS] = {
		.symbol = "instructions",
		.alias  = "",
	},
	[PERF_COUNT_HW_CACHE_REFERENCES] = {
		.symbol = "cache-references",
		.alias  = "",
	},
	[PERF_COUNT_HW_CACHE_MISSES] = {
		.symbol = "cache-misses",
		.alias  = "",
	},
	[PERF_COUNT_HW_BRANCH_INSTRUCTIONS] = {
		.symbol = "branch-instructions",
		.alias  = "branches",
	},
	[PERF_COUNT_HW_BRANCH_MISSES] = {
		.symbol = "branch-misses",
		.alias  = "",
	},
	[PERF_COUNT_HW_BUS_CYCLES] = {
		.symbol = "bus-cycles",
		.alias  = "",
	},
	[PERF_COUNT_HW_STALLED_CYCLES_FRONTEND] = {
		.symbol = "stalled-cycles-frontend",
		.alias  = "idle-cycles-frontend",
	},
	[PERF_COUNT_HW_STALLED_CYCLES_BACKEND] = {
		.symbol = "stalled-cycles-backend",
		.alias  = "idle-cycles-backend",
	},
	[PERF_COUNT_HW_REF_CPU_CYCLES] = {
		.symbol = "ref-cycles",
		.alias  = "",
	},
};

/* -------------------------------------------------------------------------- */

static struct pmu_event_s *
pmu_event_get(struct perf_s *p, struct pmu_s *pmu,
	      const char *event_name, const char *event_value)
{
	struct rbn *rbn;
	struct pmu_event_s *pmu_event;
	struct pmu_format_s *pmu_format;
	int i, n, len, off, rc;
	const char *s;
	char term_name[256];
	char path[PATH_MAX];
	int64_t term_value;
	unsigned long long *u;
	struct stat st;

	rbn = rbt_find(&pmu->events_rbt, event_name);
	if (rbn) {
		pmu_event = container_of(rbn, struct pmu_event_s, rbn);
		goto out;
	}

	/* event_value contains "TERM1=VAL1[,TERM2=VAL2...]" (e.g.
	 * "event=0x64,umask=0x09" or "event=0xc0").
	 * "/sys/bus/event_source/devices/<PMU>/format/<TERM>" contains
	 * single "config[123]?:<BIT_RANGES>" (e.g. "config:0-7,32-37" or
	 * "config1:0-15") describing how we shall assign
	 * config fields in the attr.
	 *
	 * For more information, see
	 * "linux/Documentation/ABI/testing/sysfs-bus-event_source-devices-events" and
	 * "linux/Documentation/ABI/testing/sysfs-bus-event_source-devices-format"
	 */

	n = 1;
	for (s = event_value; *s; s++) {
		if (*s == ',')
			n++;
	}

	pmu_event = calloc(1, sizeof(*pmu_event) + n*sizeof(pmu_event->terms[0]));
	if (unlikely(!pmu_event)) {
		_ERROR(p, "Not enough memory.\n");
		goto out;
	}

	pmu_event->attr.type = pmu->type;
	pmu_event->attr.size = sizeof(pmu_event->attr);
	pmu_event->n_terms = n;
	pmu_event->pmu = pmu;

	len = snprintf(pmu_event->name, sizeof(pmu_event->name), "%s", event_name);
	if (unlikely((size_t)len >= sizeof(pmu_event->name))) {
		_ERROR(p, "event name too long\n");
		goto err;

	}
	snprintf(pmu_event->event_value, sizeof(pmu_event->event_value), "%s", event_value);

	i = 0;
	s = event_value;
	while (*s) {
		if (is_sep[(unsigned char)*s]) {
			s++;
			continue;
		}
		off = 0;
		sscanf(s, "%255[^=]=%lx%n", term_name, &term_value, &off);
		if (!off) {
			_ERROR(p, "Event '%s/%s/' has bad term value: %s\n", pmu->name, event_name, s);
			goto err;
		}
		pmu_format = pmu_event->terms[i].format = pmu_format_find(p, pmu, term_name);
		pmu_event->terms[i].value = term_value;
		if (!pmu_format) {
			_ERROR(p, "Cannot find term format '%s' for event '%s/%s/'", term_name, pmu->name, event_name);
			goto err;
		}
		pmu_event->terms[i].format = pmu_format;
		switch (pmu_format->config) {
		case 0:
			u = &pmu_event->attr.config;
			break;
		case 1:
			u = &pmu_event->attr.config1;
			break;
		#ifdef PERF_ATTR_SIZE_VER1
		case 2:
			u = &pmu_event->attr.config2;
			break;
		#endif

		#ifdef PERF_ATTR_SIZE_VER8
		case 3:
			u = &pmu_event->attr.config3;
			break;
		#endif

		default:
			_ERROR(p, "Unsupported perf_attr config%d\n", pmu_format->config);
			goto err;
		}

		/* assign term value to attr config's */
		pmu_format_value_set(pmu_format->mask, u, term_value);

		s += off;
	}

	len = snprintf(path, sizeof(path),
			"/sys/bus/event_source/devices/%s/events/%s.scale",
			pmu->name, pmu_event->name);
	if (unlikely((size_t)len >= sizeof(path))) {
		_ERROR(p, "Path too long\n");
		goto err;
	}

	rc = lstat(path, &st);
	if (0 == rc) {
		/* scale file existed */
		rc = xread_double(p, path, &pmu_event->scale);
		if (rc) /* error already logged */
			goto err;
	}

	rbn_init(&pmu_event->rbn, pmu_event->name);
	rbt_ins(&pmu->events_rbt, &pmu_event->rbn);

	goto out;

 err:
	free(pmu_event);
	pmu_event = NULL;

 out:
	return pmu_event;
}

static const char *json_value_find_cstr(struct perf_s *p, json_entity_t e,
					const char *attr_name)
{
	const char *ret;
	json_entity_t val = json_value_find(e, attr_name);
	if (!val) {
		_ERROR(p, "json attribute '%s' not found.\n", attr_name);
		errno = ENOENT;
		return NULL;
	}
	ret = json_value_cstr(val); /* json_value_cstr() checks the type */
	if (!ret) {
		_ERROR(p, "json attribute '%s' type is not string.\n", attr_name);
		errno = EINVAL;
		return NULL;
	}
	return ret;
}

struct perf_hw_event_s {
	const char *name;
	struct perf_event_attr attr;
};

#if 0
	PERF_COUNT_HW_CPU_CYCLES		= 0,
	PERF_COUNT_HW_INSTRUCTIONS		= 1,
	PERF_COUNT_HW_CACHE_REFERENCES		= 2,
	PERF_COUNT_HW_CACHE_MISSES		= 3,
	PERF_COUNT_HW_BRANCH_INSTRUCTIONS	= 4,
	PERF_COUNT_HW_BRANCH_MISSES		= 5,
	PERF_COUNT_HW_BUS_CYCLES		= 6,
	PERF_COUNT_HW_STALLED_CYCLES_FRONTEND	= 7,
	PERF_COUNT_HW_STALLED_CYCLES_BACKEND	= 8,
	PERF_COUNT_HW_REF_CPU_CYCLES		= 9,
#endif

struct perf_hw_event_s hw_events[] = {
	{"cpu-cycles", { .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_CPU_CYCLES }},
	{"cycles", { .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_CPU_CYCLES }},

	{"instructions", { .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_INSTRUCTIONS }},

	{"cache-references", { .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_CACHE_REFERENCES }},

	{"cache-misses", { .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_CACHE_MISSES }},

	{"branch-instructions", { .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_BRANCH_INSTRUCTIONS }},
	{"branches", { .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_BRANCH_INSTRUCTIONS }},

	{"branch-misses", { .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_BRANCH_MISSES }},

	{"bus-cycles", { .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_BUS_CYCLES }},

	{"stalled-cycles-frontend", { .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_STALLED_CYCLES_FRONTEND }},
	{"idle-cycles-frontend", { .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_STALLED_CYCLES_FRONTEND }},

	{"stalled-cycles-backend", { .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_STALLED_CYCLES_BACKEND }},
	{"idle-cycles-backend", { .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_STALLED_CYCLES_BACKEND }},

	{"ref-cycles", { .type = PERF_TYPE_HARDWARE, .config = PERF_COUNT_HW_REF_CPU_CYCLES }},
	{0}
};

static int load_perf_hw_events(struct perf_s *p)
{
	/* load events with attr.type == PERF_TYPE_HARDWARE into our event index */
	struct perf_hw_event_s *h;
	struct pmu_event_s *pmu_event;
	int rc;

	char buf[512];

	for (h = &hw_events[0]; h->name; h++) {
		pmu_event = calloc(1, sizeof(*pmu_event));
		if (!pmu_event)
			return ENOMEM;
		pmu_event->pmu = NULL;
		snprintf(pmu_event->name, sizeof(pmu_event->name), "%s", h->name);
		pmu_event->attr = h->attr;
		pmu_event->attr.size = sizeof(h->attr);
		rbn_init(&pmu_event->rbn, pmu_event->name);
		rbt_ins(&p->builtin_events_rbt, &pmu_event->rbn);

		/* index the event with "cpu/<EVENT>" and "<EVENT>" */

		snprintf(buf, sizeof(buf), "cpu/%s/", h->name);
		rc = pmu_event_index(p, pmu_event, buf);
		if (rc)
			return rc;

		rc = pmu_event_index(p, pmu_event, h->name);
		if (rc)
			return rc;
	}

	return 0;
}

/* --- from linux/tools/perf/util/evsel.c ----------------------------------- */
/*     (with some modification) */
#define EVSEL__MAX_ALIASES 8
const char *const evsel__hw_cache[PERF_COUNT_HW_CACHE_MAX][EVSEL__MAX_ALIASES] = {
 { "L1-dcache",	"l1-d",		"l1d",		"L1-data",		},
 { "L1-icache",	"l1-i",		"l1i",		"L1-instruction",	},
 { "LLC",	"L2",							},
 { "dTLB",	"d-tlb",	"Data-TLB",				},
 { "iTLB",	"i-tlb",	"Instruction-TLB",			},
 { "branch",	"branches",	"bpu",		"btb",		"bpc",	},
 { "node",								},
};

const char *const evsel__hw_cache_op[PERF_COUNT_HW_CACHE_OP_MAX][EVSEL__MAX_ALIASES] = {
 { "load",	"loads",	"read",					},
 { "store",	"stores",	"write",				},
 { "prefetch",	"prefetches",	"speculative-read", "speculative-load",	},
};

const char *const evsel__hw_cache_result[PERF_COUNT_HW_CACHE_RESULT_MAX][EVSEL__MAX_ALIASES] = {
 { "refs",	"Reference",	"ops",		"access",		},
 { "misses",	"miss",							},
};

#define C(x)		PERF_COUNT_HW_CACHE_##x
#define CACHE_READ	(1 << C(OP_READ))
#define CACHE_WRITE	(1 << C(OP_WRITE))
#define CACHE_PREFETCH	(1 << C(OP_PREFETCH))
#define COP(x)		(1 << x)

/*
 * cache operation stat
 * L1I : Read and prefetch only
 * ITLB and BPU : Read-only
 */
static const unsigned long evsel__hw_cache_stat[C(MAX)] = {
 [C(L1D)]	= (CACHE_READ | CACHE_WRITE | CACHE_PREFETCH),
 [C(L1I)]	= (CACHE_READ | CACHE_PREFETCH),
 [C(LL)]	= (CACHE_READ | CACHE_WRITE | CACHE_PREFETCH),
 [C(DTLB)]	= (CACHE_READ | CACHE_WRITE | CACHE_PREFETCH),
 [C(ITLB)]	= (CACHE_READ),
 [C(BPU)]	= (CACHE_READ),
 [C(NODE)]	= (CACHE_READ | CACHE_WRITE | CACHE_PREFETCH),
};

static int __is_cache_op_valid(int type, int op)
{
	if (evsel__hw_cache_stat[type] & COP(op))
		return true;	/* valid */
	else
		return false;	/* invalid */
}

int __hw_cache_type_op_res_name(int type, int op, int result, char *bf, size_t size)
{
	if (result) {
		return snprintf(bf, size, "%s-%s-%s", evsel__hw_cache[type][0],
				 evsel__hw_cache_op[op][0],
				 evsel__hw_cache_result[result][0]);
	}

	return snprintf(bf, size, "%s-%s", evsel__hw_cache[type][0],
			 evsel__hw_cache_op[op][1]);
}
/* -------------------------------------------------------------------------- */

static int __is_event_supported(int type, int64_t config)
{
	struct perf_event_attr attr = {
			.type = type,
			.size = sizeof(attr),
			.config = config,
			.disabled = 1,
	};
	int fd;

	fd = perf_event_open(&attr, 0, -1, -1, 0);

	if (fd < 0) {
		/* try exclude kernel */
		attr.exclude_kernel = 1;
		fd = perf_event_open(&attr, 0, -1, -1, 0);
	}

	if (fd < 0) {
		/* try exclude guest */
		attr.exclude_guest = 1;
		fd = perf_event_open(&attr, 0, -1, -1, 0);
	}

	if (fd < 0) /* not supported */
		return 0;

	close(fd);
	return 1; /* supported */
}

struct cache_alias_s {
	int type;
	int op;
	int res;
	const char *const *type_a;
	const char *const *op_a;
	const char *const *res_a;
	int type_i;
	int op_i;
	int res_i;
};

void cache_alias_init(struct cache_alias_s *itr, int type, int op, int res)
{
	itr->type = type;
	itr->op = op;
	itr->res = res;
	itr->type_a = evsel__hw_cache[type];
	itr->op_a = evsel__hw_cache_op[op];
	itr->res_a = evsel__hw_cache_result[res];
	itr->type_i = 0;
	itr->op_i = 0;
	itr->res_i = 0;
}

int cache_alias_first(struct cache_alias_s *itr, char *buf, size_t bufsz)
{
	int len;

	itr->type_i = 0;
	itr->op_i = 0;
	itr->res_i = 0;

	len = (itr->res)?
		snprintf(buf, bufsz, "%s-%s-%s",
				evsel__hw_cache[itr->type][itr->type_i],
				evsel__hw_cache_op[itr->op][itr->op_i],
				evsel__hw_cache_result[itr->res][itr->res_i]):
		snprintf(buf, bufsz, "%s-%s",
				evsel__hw_cache[itr->type][itr->type_i],
				evsel__hw_cache_op[itr->op][itr->op_i]);

	return len;
}

int cache_alias_next(struct cache_alias_s *itr, char *buf, size_t bufsz)
{
	int len;

	if (itr->res) {
		if (!itr->res_a[++itr->res_i])
			itr->res_i = 0; /* wrap */
	}

	if (0 == itr->res_i) {
		if (!itr->op_a[++itr->op_i])
			itr->op_i = 0;
	}

	if (0 == itr->op_i) {
		if (!itr->type_a[++itr->type_i]) {
			itr->type_i = 0;
			/* end of iteration */
			return -ENOENT;
		}
	}

	len = (itr->res)?
		snprintf(buf, bufsz, "%s-%s-%s",
				evsel__hw_cache[itr->type][itr->type_i],
				evsel__hw_cache_op[itr->op][itr->op_i],
				evsel__hw_cache_result[itr->res][itr->res_i]):
		snprintf(buf, bufsz, "%s-%s",
				evsel__hw_cache[itr->type][itr->type_i],
				evsel__hw_cache_op[itr->op][itr->op_i]);

	return len;
}

static int load_perf_cache_events(struct perf_s *p)
{
	/*
	 * Populate cache events using logic influenced by
	 * "linux/tools/perf/util/print-events.c:print_hwcache_event()".
	 */

	/* load events with attr.type == PERF_TYPE_HW_CACHE into our event index */

	int ctype, op, res, len, rc;
	int64_t config;
	struct pmu_event_s *pmu_event;
	struct cache_alias_s itr;
	char buf[512];

	for (ctype = 0; ctype < PERF_COUNT_HW_CACHE_MAX; ctype++) {
		for (op = 0; op < PERF_COUNT_HW_CACHE_OP_MAX; op++) {
			if (!__is_cache_op_valid(ctype, op))
				continue;
			for (res = 0; res < PERF_COUNT_HW_CACHE_RESULT_MAX; res++) {
				config = (ctype) | (op << 8) | (res << 16) ;
				if (!__is_event_supported(PERF_TYPE_HW_CACHE, config))
					continue;
				pmu_event = calloc(1, sizeof(*pmu_event));
				if (unlikely(!pmu_event)) {
					_ERROR(p, "Not enough memory\n");
					return ENOMEM;
				}
				len = __hw_cache_type_op_res_name(ctype, op, res,
						pmu_event->name,
						sizeof(pmu_event->name));
				if (unlikely((size_t)len >= sizeof(pmu_event->name))) {
					_ERROR(p, "cache name too long\n");
					free(pmu_event);
					return ENAMETOOLONG;
				}
				pmu_event->attr.size = sizeof(pmu_event->attr);
				pmu_event->attr.type = PERF_TYPE_HW_CACHE;
				pmu_event->attr.config = config;
				pmu_event->pmu = NULL;
				rbn_init(&pmu_event->rbn, pmu_event->name);
				rbt_ins(&p->builtin_events_rbt, &pmu_event->rbn);

				cache_alias_init(&itr, ctype, op, res);
				snprintf(buf, 5, "cpu/");

				for (len = cache_alias_first(&itr, buf+4, sizeof(buf)-4);
				     len > 0;
				     len = cache_alias_next(&itr, buf+4, sizeof(buf)-4)) {
					/* "<NAME>" */
					rc = pmu_event_index(p, pmu_event, buf + 4);
					if (rc)
						return rc;
					/* "cpu/<NAME>/" */
					snprintf(buf+4+len, 2, "/");
					rc = pmu_event_index(p, pmu_event, buf);
					if (rc)
						return rc;
				}
			}
		}
	}

	return 0;
}

static struct pmu_event_s *event_lookup(struct perf_s *p, const char *name)
{
	struct rbn *rbn;
	struct pmu_s *pmu;
	struct pmu_event_s *pmu_event = NULL;
	struct pmu_event_idx_s *pe_idx;
	json_entity_t e_model_event;
	const char *c_pmu, *c_event, *c_name;

	int n, off;
	int is_pe_form = 0; /* name is "/<PMU>/<EVENT>/" format */
	char path[PATH_MAX];
	char pmu_name[256], event_name[256];
	char event_value[512]; /* e.g. "event=0x29,umask=7" */


	/* If the event has already been looked up, return it. */
	rbn = rbt_find(&p->pmu_event_idx_rbt, name);
	if (rbn) {
		pe_idx = container_of(rbn, struct pmu_event_idx_s, rbn);
		pmu_event = pe_idx->pmu_event;
		goto out;
	}

	off = 0;
	sscanf(name, "%255[^/]/%255[^/]/%n", pmu_name, event_name, &off);
	if ((size_t)off == strlen(name)) {
		is_pe_form = 1;
	}

	/* NOTE: event lookup order
	 *       1) look into model_events dict
	 *       2) look into "/sys"
	 *          2.1) if it is in "<PMU>/<EVENT>/" form, look into
	 *              "/sys/bus/event_source/devices/<PMU>/events/<EVENT>", or
	 *              "/sys/bus/event_source/devices/<PMU>_<NUM>/events/<EVENT>"
	 *          2.2) if it is in "<EVENT>" form (no "/"), look into
	 *               "/sys/bus/event_source/devices/cpu/events/<EVENT>".
	 *       3) built-in perf events
	 *       4) legacy cache form "<CACHE_TYPE>-<A>-<B>".
	 */

	/* 1) look into model event dict */
	e_model_event = p->model_events?json_value_find(p->model_events, name):NULL;
	if (!e_model_event)
		goto try_sysfs;

	if (json_entity_type(e_model_event) != JSON_DICT_VALUE) {
		_ERROR(p, "perfdb event is not an object.\n");
		errno = EINVAL;
		goto out;
	}
	c_pmu = json_value_find_cstr(p, e_model_event, "pmu");
	if (!c_pmu)
		goto out;
	c_name = json_value_find_cstr(p, e_model_event, "name");
	if (!c_name)
		goto out;
	c_event = json_value_find_cstr(p, e_model_event, "event");
	if (!c_event)
		goto out;

	n = snprintf(pmu_name, sizeof(pmu_name), "%s", c_pmu);
	if (unlikely((size_t)n >= sizeof(pmu_name))) {
		_ERROR(p, "perfdb event 'pmu' value too long.\n");
		errno = ENAMETOOLONG;
		goto out;
	}

	n = snprintf(event_name, sizeof(event_name), "%s", c_name);
	if (unlikely((size_t)n >= sizeof(event_name))) {
		_ERROR(p, "perfdb event 'name' value too long.\n");
		errno = ENAMETOOLONG;
		goto out;
	}

	n = snprintf(event_value, sizeof(event_value), "%s", c_event);
	if (unlikely((size_t)n >= sizeof(event_value))) {
		_ERROR(p, "perfdb event 'event' attribute value too long.\n");
		errno = ENOBUFS;
		goto out;
	}
	goto process_event;

 try_sysfs:
	/* 2) look into /sys fs */
	if (!is_pe_form)
		goto event_not_found;
	/* pmu_name and event_name have alread been populated. */
	n = snprintf(path, sizeof(path), "/sys/bus/event_source/devices/%s/events/%s", pmu_name, event_name);
	if (unlikely((size_t)n >= sizeof(path))) {
		errno = ENAMETOOLONG;
		_ERROR(p, "event name too long.\n");
		goto out;
	}
	n = xread_buf(p, path, event_value, sizeof(event_value));
	if (n < 0) {
		/* Event not found in both perfdb and /sys; errno is already set */
		_ERROR(p, "Cannot resolve event '%s'\n", name);
		goto out;
	}
	if (unlikely((size_t)n >= sizeof(event_value))) {
		errno = ENOBUFS;
		_ERROR(p, "Event value from '%s' fs is too large.\n", path);
		goto out;
	}
	goto process_event;

 process_event:

	pmu = pmu_find(p, pmu_name);
	if (!pmu) /* errno is set and err is also logged */
		goto out;
	pmu_event = pmu_event_get(p, pmu, event_name, event_value);
	if (!pmu_event) /* errno is set and err is also logged */
		goto out;

	/* indexing the result */
	pmu_event_index(p, pmu_event, name);
	goto out;

 event_not_found:
	 _ERROR(p, "Event '%s' not found\n", name);
	 errno = ENOENT;

 out:
	return pmu_event;
}

static struct counter_s *
counter_new(struct perf_s *p, struct pmu_event_s *pmu_event, int cpu, int pid,
	    int instance_idx, json_entity_t conf_event, struct event_rec_s *event_rec)
{
	struct counter_s *c;
	(void)p;
	c = calloc(1, sizeof(*c));
	if (!c)
		goto out;
	c->pmu_event = pmu_event;
	c->pid = pid;
	c->cpu = cpu;
	c->instance_idx = instance_idx;
	c->fd = -1;
	c->attr = pmu_event->attr;
	c->conf_event = conf_event;
	c->event_rec = event_rec;
 out:
	return c;
}

static int event_is_pe_form(const char *name, char *pmu_name, size_t pmu_sz,
			    char *event_name, size_t event_sz)
{
	int off = 0;
	char pmu[256] = "";
	char event[256] = "";

	sscanf(name, "%255[^/]/%255[^/]/%n", pmu, event, &off);
	if (!off || (size_t)off != strlen(name))
		return 0;
	snprintf(pmu_name, pmu_sz, "%s", pmu);
	snprintf(event_name, event_sz, "%s", event);
	return 1;
}

static struct pmu_event_s *
event_lookup_for_instance(struct perf_s *p, const char *name, struct instance_s *inst)
{
	char pmu_name[256];
	char event_name[256];
	char full_name[512];

	if (p->template_type != TEMPLATE_PMU)
		return event_lookup(p, name);

	if (event_is_pe_form(name, pmu_name, sizeof(pmu_name),
			     event_name, sizeof(event_name))) {
		if (0 != strcmp(pmu_name, inst->pmu)) {
			errno = EINVAL;
			_ERROR(p, "Event '%s' targets PMU '%s', not template instance '%s'.\n",
			       name, pmu_name, inst->pmu);
			return NULL;
		}
		return event_lookup(p, name);
	}

	snprintf(full_name, sizeof(full_name), "%s/%s/", inst->pmu, name);
	return event_lookup(p, full_name);
}

static int counter_cpu_for_instance(struct perf_s *p, struct instance_s *inst,
				    uint8_t *event_cpu_mask,
				    struct pmu_event_s *pmu_event)
{
	int cpu;
	uint8_t *pmu_mask;
	uint8_t all[MAX_CPU];

	if (pmu_event->pmu) {
		pmu_mask = pmu_event->pmu->cpu_mask;
	} else if (p->template_type == TEMPLATE_CPU) {
		cpu_mask_all(p, all);
		pmu_mask = all;
	} else {
		errno = EINVAL;
		_ERROR(p, "Built-in/core event '%s' is only valid with a CPU template.\n",
		       pmu_event->name);
		return -1;
	}

	if (p->template_type == TEMPLATE_CPU) {
		cpu = inst->representative_cpu;
		if (cpu >= 0 && cpu < MAX_CPU && event_cpu_mask[cpu] && pmu_mask[cpu])
			return cpu;
		errno = EINVAL;
		return -1;
	}

	for (cpu = 0; cpu < p->n_cpu && cpu < MAX_CPU; cpu++) {
		if (inst->cpu_mask[cpu] && event_cpu_mask[cpu] && pmu_mask[cpu])
			return cpu;
	}
	errno = EINVAL;
	return -1;
}

static struct event_rec_s *
event_rec_new(struct perf_s *p, const char *event_name, int pid, json_entity_t conf_event)
{
	struct event_rec_s *er;

	er = calloc(1, sizeof(*er));
	if (!er)
		return NULL;
	snprintf(er->name, sizeof(er->name), "%s", event_name);
	snprintf(er->event_spec, sizeof(er->event_spec), "%s", event_name);
	snprintf(er->pmu, sizeof(er->pmu), "%s", template_type_name(p->template_type));
	er->pid = pid;
	er->conf_event = conf_event;
	TAILQ_INSERT_TAIL(&p->events, er, entry);
	return er;
}

static int process_conf_event(struct perf_s *p, json_entity_t e)
{
	json_entity_t e_cpu, e_pid, e_event;
	enum json_value_e e_type;
	int pid = -1, rc, i, cpu;
	const char *e_event_str;
	struct pmu_event_s *pmu_event;
	struct counter_s *counter;
	struct event_rec_s *er;
	uint8_t event_cpu_mask[MAX_CPU];
	int is_scaled = -1;

	e_type = json_entity_type(e);
	if (e_type != JSON_DICT_VALUE) {
		_ERROR(p, "Expecting an object but got: %s\n", json_type_name(e_type));
		return EINVAL;
	}

	e_cpu = json_value_find(e, "cpu");
	e_pid = json_value_find(e, "pid");
	e_event = json_value_find(e, "event");

	if (!e_event) {
		_ERROR(p, "'event' attribute is required\n");
		return EINVAL;
	}
	if (JSON_STRING_VALUE != json_entity_type(e_event)) {
		_ERROR(p, "'event' must be a string\n");
		return EINVAL;
	}
	e_event_str = json_value_cstr(e_event);
	if (!e_event_str[0]) {
		_ERROR(p, "'event' must not be empty\n");
		return EINVAL;
	}

	if (e_pid) {
		e_type = json_entity_type(e_pid);
		switch (e_type) {
		case JSON_INT_VALUE:
			pid = json_value_int(e_pid);
			break;
		case JSON_STRING_VALUE:
			pid = strtol(json_value_cstr(e_pid), NULL, 0);
			break;
		default:
			_ERROR(p, "Bad 'pid' value\n");
			return EINVAL;
		}
	}

	cpu_mask_all(p, event_cpu_mask);
	if (e_cpu) {
		rc = cpu_mask_from_json(p, e, "cpu", event_cpu_mask);
		if (rc)
			return rc;
	}

	er = event_rec_new(p, e_event_str, pid, e);
	if (!er)
		return ENOMEM;

	for (i = 0; i < p->instance_count; i++) {
		pmu_event = event_lookup_for_instance(p, e_event_str, &p->instances[i]);
		if (!pmu_event)
			goto err;
		cpu = counter_cpu_for_instance(p, &p->instances[i], event_cpu_mask, pmu_event);
		if (cpu < 0) {
			_ERROR(p, "Event '%s' is not compatible with template instance '%s'.\n",
			       e_event_str, p->instances[i].name);
			goto err;
		}
		if (is_scaled < 0) {
			is_scaled = !!pmu_event->scale;
			er->is_scaled = is_scaled;
			er->pmu_event = pmu_event;
			snprintf(er->pmu, sizeof(er->pmu), "%s",
				 pmu_event->pmu ? pmu_event->pmu->name : "builtin");
		} else if (is_scaled != !!pmu_event->scale) {
			errno = EINVAL;
			_ERROR(p, "Event '%s' resolves to mixed scaled/unscaled PMU events.\n",
			       e_event_str);
			goto err;
		} else if (pmu_event->pmu &&
			   0 != strcmp(er->pmu, pmu_event->pmu->name)) {
			snprintf(er->pmu, sizeof(er->pmu), "per-instance");
		}
		counter = counter_new(p, pmu_event, cpu, pid, i, e, er);
		if (!counter) {
			errno = ENOMEM;
			goto err;
		}
		TAILQ_INSERT_TAIL(&p->counters, counter, entry);
		p->n_counters++;
	}

	if (er->is_scaled)
		p->n_sevents++;
	else
		p->n_events++;
	return 0;
err:
	_ERROR(p, "Failed to process event '%s': %s\n", e_event_str, strerror(errno));
	return errno;
}

static int make_events(struct perf_s *p)
{
	/* making events according to p->config */
	/* Populating p->counters */
	json_entity_t e_events;
	json_entity_t e;
	int len, rc;
	json_entity_t config = json_doc_root(p->config);
	enum json_value_e etype;

	etype = json_entity_type(config);
	if (etype != JSON_DICT_VALUE) {
		_ERROR(p, "Expecting a dictionary, but got: %s\n", json_type_name(etype));
		return EINVAL;
	}

	e_events = json_value_find(config, "events");
	if (!e_events) {
		_ERROR(p, "Missing 'events' attribute in config dict.\n");
		return ENOENT;
	}
	etype = json_entity_type(e_events);
	if (etype != JSON_LIST_VALUE) {
		_ERROR(p, "'events' attribute must be a list of events, but got: %s\n", json_type_name(etype));
		return EINVAL;
	}

	len = 0;
	for (e = json_item_first(e_events); e; e = json_item_next(e)) {
		rc = process_conf_event(p, e);
		if (rc)
			goto out;
		len += 1;
	}
	_DEBUG(p, "number of events from config: %d\n", len);

	rc = 0;
 out:
	return rc;
}

static int make_set(struct perf_s *p)
{
	int rc, i;
	ldms_schema_t schema;
	ldms_record_t inst_recdef = NULL, recdef = NULL, srecdef = NULL;
	ldms_mval_t rec_mval, mval;
	ssize_t rec_sz;

	enum {
		INST_INDEX,
		INST_TYPE,
		INST_NAME,
		INST_PMU,
		INST_CPU,
		INST_SOCKET,
		INST_DIE,
		INST_NODE,
		INST_CACHE_LEVEL,
		INST_CACHE_ID,
	};
	enum {
		EV_NAME,
		EV_SPEC,
		EV_PMU,
		EV_PID,
		EV_VALUES,
	};
	struct ldms_metric_template_s insttmp[] = {
		{ "index", 0, LDMS_V_U32, NULL, 1, NULL },
		{ "instance_type", 0, LDMS_V_CHAR_ARRAY, NULL, 32, NULL },
		{ "name", 0, LDMS_V_CHAR_ARRAY, NULL, 128, NULL },
		{ "pmu", 0, LDMS_V_CHAR_ARRAY, NULL, 128, NULL },
		{ "representative_cpu", 0, LDMS_V_S32, NULL, 1, NULL },
		{ "socket_id", 0, LDMS_V_S32, NULL, 1, NULL },
		{ "die_id", 0, LDMS_V_S32, NULL, 1, NULL },
		{ "node_id", 0, LDMS_V_S32, NULL, 1, NULL },
		{ "cache_level", 0, LDMS_V_S32, NULL, 1, NULL },
		{ "cache_id", 0, LDMS_V_S32, NULL, 1, NULL },
		{0}
	};
	struct ldms_metric_template_s rectmp[] = {
		{ "name", 0, LDMS_V_CHAR_ARRAY, NULL, 128, NULL },
		{ "event_spec", 0, LDMS_V_CHAR_ARRAY, NULL, PMU_EVENT_VALUE_SZ, NULL },
		{ "pmu", 0, LDMS_V_CHAR_ARRAY, NULL, 128, NULL },
		{ "pid", 0, LDMS_V_S64, NULL, 1, NULL },
		{ "values", 0, LDMS_V_S64_ARRAY, NULL, p->instance_count, NULL },
		{0}
	};
	struct ldms_metric_template_s srectmp[] = {
		{ "name", 0, LDMS_V_CHAR_ARRAY, NULL, 128, NULL },
		{ "event_spec", 0, LDMS_V_CHAR_ARRAY, NULL, PMU_EVENT_VALUE_SZ, NULL },
		{ "pmu", 0, LDMS_V_CHAR_ARRAY, NULL, 128, NULL },
		{ "pid", 0, LDMS_V_S64, NULL, 1, NULL },
		{ "values", 0, LDMS_V_D64_ARRAY, NULL, p->instance_count, NULL },
		{0}
	};
	struct counter_s *c;
	struct event_rec_s *er;
	struct instance_s *inst;

	schema = base_schema_new(p->base);
	if (!schema) {
		rc = errno;
		goto err_0;
	}

	inst_recdef = ldms_record_from_template("instance_recdef", insttmp, NULL);
	if (!inst_recdef) {
		rc = errno;
		goto err_1;
	}
	rc = ldms_schema_record_add(schema, inst_recdef);
	if (rc < 0) {
		rc = -rc;
		goto err_2;
	}
	p->instance_recdef_midx = rc;

	recdef = ldms_record_from_template("counter_recdef", rectmp, NULL);
	if (!recdef) {
		rc = errno;
		goto err_1;
	}
	rc = ldms_schema_record_add(schema, recdef);
	if (rc < 0) {
		rc = -rc;
		ldms_record_delete(recdef);
		goto err_1;
	}
	p->counter_recdef_midx = rc;

	srecdef = ldms_record_from_template("scaled_counter_recdef", srectmp, NULL);
	if (!srecdef) {
		rc = errno;
		goto err_1;
	}
	rc = ldms_schema_record_add(schema, srecdef);
	if (rc < 0) {
		rc = -rc;
		ldms_record_delete(srecdef);
		goto err_1;
	}
	p->scounter_recdef_midx = rc;

	rec_sz = ldms_record_heap_size_get(inst_recdef);
	rc = ldms_schema_metric_list_add(schema, "instances", NULL,
					 p->instance_count * rec_sz);
	if (rc < 0) {
		rc = -rc;
		goto err_4;
	}
	p->instances_midx = rc;

	/* list of counters */
	rec_sz = ldms_record_heap_size_get(recdef);
	rc = ldms_schema_metric_list_add(schema, "counters", NULL, p->n_events * rec_sz);
	if (rc < 0) {
		rc = -rc;
		goto err_4;
	}
	p->counters_midx = rc;

	/* list of scaled counters */
	rec_sz = ldms_record_heap_size_get(srecdef);
	rc = ldms_schema_metric_list_add(schema, "scaled_counters", NULL, p->n_sevents * rec_sz);
	if (rc < 0) {
		rc = -rc;
		goto err_4;
	}
	p->scounters_midx = rc;

	p->set = base_set_new(p->base);
	if (!p->set) {
		_ERROR(p, "base_set_new() failed\n");
		rc = errno;
		goto err_4;
	}
	p->counters_mval = ldms_metric_get(p->set, p->counters_midx);
	p->scounters_mval = ldms_metric_get(p->set, p->scounters_midx);
	p->instances_mval = ldms_metric_get(p->set, p->instances_midx);

	ldms_transaction_begin(p->set);

	for (i = 0; i < p->instance_count; i++) {
		inst = &p->instances[i];
		rec_mval = ldms_record_alloc(p->set, p->instance_recdef_midx);
		if (!rec_mval) {
			_ERROR(p, "Cannot allocate instance record\n");
			rc = errno;
			goto err_5;
		}
		ldms_list_append_record(p->set, p->instances_mval, rec_mval);
		ldms_record_metric_get(rec_mval, INST_INDEX)->v_u32 = inst->index;
		snprintf(ldms_record_metric_get(rec_mval, INST_TYPE)->a_char, 32, "%s",
			 template_type_name(inst->type));
		snprintf(ldms_record_metric_get(rec_mval, INST_NAME)->a_char, 128, "%s",
			 inst->name);
		snprintf(ldms_record_metric_get(rec_mval, INST_PMU)->a_char, 128, "%s",
			 inst->pmu);
		ldms_record_metric_get(rec_mval, INST_CPU)->v_s32 = inst->representative_cpu;
		ldms_record_metric_get(rec_mval, INST_SOCKET)->v_s32 = inst->socket_id;
		ldms_record_metric_get(rec_mval, INST_DIE)->v_s32 = inst->die_id;
		ldms_record_metric_get(rec_mval, INST_NODE)->v_s32 = inst->node_id;
		ldms_record_metric_get(rec_mval, INST_CACHE_LEVEL)->v_s32 = inst->cache_level;
		ldms_record_metric_get(rec_mval, INST_CACHE_ID)->v_s32 = inst->cache_id;
	}

	TAILQ_FOREACH(er, &p->events, entry) {
		int recdef_midx;
		ldms_mval_t list_mval;

		if (er->is_scaled) {
			recdef_midx = p->scounter_recdef_midx;
			list_mval = p->scounters_mval;
		} else {
			recdef_midx = p->counter_recdef_midx;
			list_mval = p->counters_mval;
		}

		rec_mval = ldms_record_alloc(p->set, recdef_midx);
		if (!rec_mval) {
			_ERROR(p, "Cannot allocate counter record\n");
			rc = errno;
			goto err_5;
		}
		ldms_list_append_record(p->set, list_mval, rec_mval);
		er->rec_mval = rec_mval;
		snprintf(ldms_record_metric_get(rec_mval, EV_NAME)->a_char, 128, "%s",
			 er->name);
		snprintf(ldms_record_metric_get(rec_mval, EV_SPEC)->a_char,
			 PMU_EVENT_VALUE_SZ, "%s", er->event_spec);
		snprintf(ldms_record_metric_get(rec_mval, EV_PMU)->a_char, 128, "%s",
			 er->pmu);
		ldms_record_metric_get(rec_mval, EV_PID)->v_s64 = er->pid;
		er->values_mval = ldms_record_metric_get(rec_mval, EV_VALUES);
	}

	TAILQ_FOREACH(c, &p->counters, entry) {
		mval = c->event_rec->values_mval;
		if (c->event_rec->is_scaled)
			c->mval = (ldms_mval_t)&mval->a_d[c->instance_idx];
		else
			c->mval = (ldms_mval_t)&mval->a_s64[c->instance_idx];
	}
	ldms_transaction_end(p->set);

	ldms_set_publish(p->set);

	return 0;
 err_5:
	base_set_delete(p->base);
	p->set = NULL;
 err_4:
	p->instance_recdef_midx = -1;
	p->instances_midx = -1;
	p->counter_recdef_midx = -1;
	p->counters_midx = -1;
	p->scounter_recdef_midx = -1;
	p->scounters_midx = -1;
	goto err_1;
 err_2:
	ldms_record_delete(inst_recdef);
 err_1:
	base_schema_delete(p->base);
 err_0:
	return rc;
}

static int perf_event_open(struct perf_event_attr *attr,
			   pid_t pid, int cpu, int group_fd,
			   unsigned long flags)
{
	return syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags);
}

static int perf_open(struct perf_s *p)
{
	/* perform perf_event_open() for counters in p->counters */
	int rc;
	struct counter_s *c;

	TAILQ_FOREACH(c, &p->counters, entry) {
		assert(c->fd < 0);
		c->fd = perf_event_open(&c->attr, c->pid, c->cpu, -1, 0);
		if (c->fd < 0) {
			_ERROR(p, "perf_event_open() failed for event '%s', errno: %d\n",
					json_value_find_cstr(p, c->conf_event, "event"),
					errno);
			rc = errno;
			goto err;
		}
	}

	return 0;

 err:
	perf_close(p);
	return rc;
}

static void perf_close(struct perf_s *p)
{
	struct counter_s *c;
	TAILQ_FOREACH(c, &p->counters, entry) {
		if (c->fd < 0)
			continue;
		close(c->fd);
		c->fd = -1;
	}
}

static int get_n_cpu(struct perf_s *p)
{
	int n_cpu;
	DIR *d;
	struct dirent *dent;

	n_cpu = sysconf(_SC_NPROCESSORS_ONLN);
	if (unlikely(n_cpu < 0)) {
		_DEBUG(p, "Cannot get number of cpus from sysconf(), try /sys/bus/cpu/devices\n");
		n_cpu = 0;
		d = opendir("/sys/bus/cpu/devices");
		while ((dent = readdir(d))) {
			if (0 == strncmp("cpu", dent->d_name, 3)) /* count only "cpu*" */
				continue;
			n_cpu++;
		}
	}
	return n_cpu;
}

static int setup_model_events(struct perf_s *p)
{
	int rc = 0;
	json_entity_t m, pmu_table;
	const char *m_model_str;
	json_entity_t perfdb;
	if (!p->perfdb)
		return ENOENT;
	perfdb = json_doc_root(p->perfdb);

	if (p->model_events)
		return 0;

	pmu_table = json_value_find(perfdb, "pmu_table");
	if (!pmu_table) {
		_ERROR(p, "'pmu_table' not found in perfdb\n");
		return ENOENT;
	}

	/* find the matching model */
	for (m = json_item_first(pmu_table); m; m = json_item_next(m)) {
		m_model_str = json_value_find_cstr(p, m, "family-model");
		if (!m_model_str)
			return EINVAL; /* already logged */
		rc = p->cpu_match(p, m_model_str);
		if (0 == rc) {
			/* match found */
			_INFO(p, "cpu_model: %s matches %s entry\n", p->cpu_model, m_model_str);
			break;
		}
	}

	if (!m) {
		_ERROR(p, "No matching CPU model found for '%s'\n", p->cpu_model);
		return ENOENT;
	}

	p->model_events = json_value_find(m, "events");
	if (!p->model_events) {
		_ERROR(p, "'events' attribute missing for model '%s'\n", m_model_str);
		return ENOENT;
	}

	return 0;
}

static int config(ldmsd_plug_handle_t handle,
		  struct attr_value_list *kwl,
		  struct attr_value_list *avl)
{
	int rc;
	char *av_inst;
	char *av_prod;
	char *av_conf;
	char *av_perfdb;
	json_entity_t e_schema;
	json_entity_t e_conf_perfdb;
	json_entity_t e_config;
	const char *c_schema;

	struct perf_s *p = ldmsd_plug_ctxt_get(handle);

	(void)kwl;
	assert(p);

	av_inst = av_value(avl, "instance");
	av_prod = av_value(avl, "producer");
	av_conf = av_value(avl, "conf");
	av_perfdb = av_value(avl, "perfdb");

	_DEBUG(p, "instance: %s\n", av_inst);
	_DEBUG(p, "producer: %s\n", av_prod);
	_DEBUG(p, "conf: %s\n", av_conf);
	_DEBUG(p, "perfdb: %s\n", av_perfdb);

	if (p->base) {
		_ERROR(p, "already configured.\n");
		rc = EBUSY;
		goto err;
	}

	if (!av_conf) {
		_ERROR(p, "'conf' parameter is required\n");
		rc = EINVAL;
		goto err;
	}

	p->config = xread_json(p, av_conf);
	if (!p->config) {
		rc = errno;
		goto err;
	}
	e_config = json_doc_root(p->config);
	e_schema = json_value_find(e_config, "schema");
	if (e_schema) {
		c_schema = json_value_cstr(e_schema);
	} else {
		c_schema = SAMP;
	}

	p->base = base_config(avl, ldmsd_plug_cfg_name_get(handle), c_schema, p->log);
	if (!p->base) {
		rc = errno;
		goto err;
	}

	e_conf_perfdb = json_value_find(e_config, "perfdb");

	if (av_perfdb) {
		rc = load_perfdb(p, av_perfdb);
		if (rc)
			goto err;
	} else if (e_conf_perfdb) {
		if (json_entity_type(e_conf_perfdb) != JSON_STRING_VALUE) {
			_ERROR(p, "'perfdb' attribute must be a string.\n");
			rc = EINVAL;
			goto err;
		}
		rc = load_perfdb(p, json_value_cstr(e_conf_perfdb));
		if (rc)
			goto err;
	} else if (default_perfdb) {
		/* no-op */
		p->perfdb = default_perfdb;
		_INFO(p, "Using default perfdb derived from `perf list hw cache pmu`.\n");
	} else {
		/* perfdb not given, and no global perfdb; give a warning */
		_WARN(p, "'perfdb' option not given. A lot of vendor-specific"
			 " events are defined in perfdb derived from pmu-events"
			 " from Linux kernel source tree. To generate a perfdb"
			 " file, please consult 'ldms-perfdb-gen --help'\n");
	}

	if (p->perfdb) {
		rc = setup_model_events(p);
		if (rc)
			goto err;
	}

	rc = template_build(p, e_config);
	if (rc)
		goto err;

	rc = make_events(p);
	if (rc)
		goto err;

	rc = make_set(p);
	if (rc)
		goto err;

	rc = perf_open(p);
	if (rc)
		goto err;

	rc = 0;

	goto out;

 err:
	perf_config_data_free(p);
	if (p->config)
		json_doc_free(p->config);
	p->config = NULL;
	if (p->perfdb && p->perfdb != default_perfdb)
		json_doc_free(p->perfdb);
	p->perfdb = NULL;
	p->model = NULL;
	p->model_events = NULL;
	if (p->base)
		base_set_delete(p->base);
	base_del(p->base);
	p->base = NULL;
 out:
	return rc;
}

static int sample(ldmsd_plug_handle_t handle)
{
	struct perf_s *p = ldmsd_plug_ctxt_get(handle);
	struct counter_s *c;
	int len;
	int64_t raw;
	if (!p || !p->set)
		return ENOENT;
	ldms_transaction_begin(p->set);
	TAILQ_FOREACH(c, &p->counters, entry) {
		len = read(c->fd, &raw, sizeof(raw));
		if (len < 0) {
			_ERROR(p, "sample(): read() error: %d\n", errno);
			continue;
		}

		if (len == 0) {
			_ERROR(p, "sample(): read() EOF\n");
			continue;
		}
		if (c->pmu_event->scale) {
			c->mval->v_d = raw * c->pmu_event->scale;
		} else {
			c->mval->v_s64 = raw;
		}
	}
	ldms_transaction_end(p->set);
	return 0;
}

int x86_proc_cpu_model(char *buf, size_t bufsz)
{
	FILE *f;
	int cpu_id = -1;
	char lbuf[256];
	char vendor[256];
	char family[64];
	char model[64];
	int n;
	int rc;

	f = fopen("/proc/cpuinfo", "r");
	if (!f)
		return ENOENT;

	fgets(lbuf, sizeof(lbuf), f);
	n = sscanf(lbuf, "processor%*[\t ]: %d", &cpu_id);
	if (n != 1) {
		rc = EINVAL;
		goto out;
	}

	fgets(lbuf, sizeof(lbuf), f);
	n = sscanf(lbuf, "vendor_id%*[\t ]: %s", vendor);
	if (n != 1) {
		rc = EINVAL;
		goto out;
	}

	fgets(lbuf, sizeof(lbuf), f);
	n = sscanf(lbuf, "cpu family%*[\t ]: %s", family);
	if (n != 1) {
		rc = EINVAL;
		goto out;
	}

	fgets(lbuf, sizeof(lbuf), f);
	n = sscanf(lbuf, "model%*[\t ]: %s", model);
	if (n != 1) {
		rc = EINVAL;
		goto out;
	}

	n = snprintf(buf, bufsz, "%s-%s-%s", vendor, family, model);
	if ((size_t)n >= bufsz) {
		rc = ENOBUFS;
		goto out;
	}

	rc = 0;
 out:
	fclose(f);
	return rc;
}

static int x86_set_cpu_model(struct perf_s *p)
{
	return x86_proc_cpu_model(p->cpu_model, sizeof(p->cpu_model));
}

static int x86_cpu_match(struct perf_s *p, const char *family_model)
{
	regex_t regex;
	char reg_str[1024];
	int rc;
	int len;
	len = snprintf(reg_str, sizeof(reg_str), "^%s$", family_model);
	if (unlikely((size_t)len >= sizeof(reg_str))) {
		_ERROR(p, "model regex too long (len: %d >= %zu)\n", len, sizeof(reg_str));
		return ENOBUFS;
	}
	rc = regcomp(&regex, reg_str, REG_EXTENDED);
	if (unlikely(rc)) {
		_ERROR(p, "setup_model_events: regcomp() error: %d on input: %s\n", rc, family_model);
		return rc;
	}
	rc = regexec(&regex, p->cpu_model, 0, NULL, 0);
	regfree(&regex);
	return rc;
}

static int arm64_set_cpu_model(struct perf_s *p)
{
	const char *sysfs = "/sys";
	char path[PATH_MAX];
	char *res;
	int i;
	FILE *f;
	for (i = 0; i < p->n_cpu; i++) {
		snprintf(path, PATH_MAX,
			 "%s/devices/system/cpu/cpu%d/regs/identification/midr_el1",
			 sysfs, i);
		f = fopen(path, "r");
		if (!f)
			continue;
		res = fgets(p->cpu_model, sizeof(p->cpu_model), f);
		fclose(f);
		if (res)
			return 0;
	}
	return ENOENT;

}

#define MIDR_VARIENT_MASK  0x00000fL /* bit 0-3 */
#define MIDR_REVISION_MASK 0xf00000L /* bit 20-23 */
#define MIDR_ID_MASK (~(MIDR_VARIENT_MASK|MIDR_REVISION_MASK))
static int arm64_cpu_match(struct perf_s *p, const char *family_model)
{
	/* NOTE: from linux/tools/perf/arch/arm64/util/header.c,
	 *       p->cpu_model matches family_model if they have the same ID
	 *       and cpu_model{var,rev} >= family_model{var,rev} (think of
	 *       {var,rev} like MAJOR.MINOR number).
	 */
	uint64_t cm = strtoul(p->cpu_model, NULL, 16);
	uint64_t fm = strtoul(family_model, NULL, 16);
	uint64_t cm_var, fm_var, cm_rev, fm_rev;

	if ((cm & MIDR_ID_MASK) != (fm & MIDR_ID_MASK))
		return -1;

	cm_var = cm & MIDR_VARIENT_MASK;
	fm_var = fm & MIDR_VARIENT_MASK;

	if (cm_var > fm_var)
		return 0;
	cm_rev = cm & MIDR_REVISION_MASK;
	fm_rev = fm & MIDR_REVISION_MASK;
	if (cm_var == fm_var && cm_rev >= fm_rev)
		return 0;
	return -1;
}

static int constructor(ldmsd_plug_handle_t handle)
{
	struct perf_s *p;
	int rc;
	p = calloc(1, sizeof(*p));
	if (!p)
		return ENOMEM;
	p->plug = handle;
	p->log = ldmsd_plug_log_get(handle);
	TAILQ_INIT(&p->counters);
	TAILQ_INIT(&p->events);

	rc = uname(&p->uname);
	if (rc) {
		_WARN(p, "uname() error: %d\n", errno);
		goto err;
	}

	if (0 == strcmp("x86_64", p->uname.machine)) {
		p->set_cpu_model = x86_set_cpu_model;
		p->cpu_match = x86_cpu_match;
	} else if (0 == strcmp("aarch64", p->uname.machine)) {
		p->set_cpu_model = arm64_set_cpu_model;
		p->cpu_match = arm64_cpu_match;
	} else {
		errno = ENOTSUP;
		goto err;
	}

	/* get number of cpus and cpu model */
	p->n_cpu = get_n_cpu(p);
	if (p->n_cpu > MAX_CPU) {
		_ERROR(p, "Only up to %d CPUs are supported, found %d.\n",
		       MAX_CPU, p->n_cpu);
		rc = EINVAL;
		goto err;
	}
	rc = p->set_cpu_model(p);
	if (rc)
		goto err;

	rbt_init(&p->pmu_rbt, rbt_str_cmp);
	rbt_init(&p->pmu_event_idx_rbt, rbt_str_cmp);
	rbt_init(&p->builtin_events_rbt, rbt_str_cmp);

	rc = load_perf_hw_events(p);
	if (rc)
		goto err;
	rc = load_perf_cache_events(p);
	if (rc)
		goto err;

	ldmsd_plug_ctxt_set(handle, p);

	return 0;
 err:
	perf_free(p);
	return rc;
}

struct ldmsd_sampler ldmsd_plugin_interface = {
	.base.type = LDMSD_PLUGIN_SAMPLER,
	.base.flags = LDMSD_PLUGIN_MULTI_INSTANCE,
	.base.config = config,
	.base.usage = usage,
	.base.constructor = constructor,
	.base.destructor = destructor,

	.sample = sample,
};

__attribute__((constructor))
static void init_once()
{
	char dir[PATH_MAX];
	char *cmd = NULL;
	char *c = NULL;
	ssize_t sz;
	FILE *pf = NULL;
	int rc;
	char *line = NULL;
	size_t line_sz;
	jbuf_t jbuf = NULL;

	sz = readlink("/proc/self/exe", dir, sizeof(dir)-1);
	if (sz < 0) {
		ovis_log(NULL, OVIS_LERROR,
			"readlink(\"/proc/self/exe\") error: %d\n", errno);
		return;
	}
	dir[sz] = '\0';
	/* Determine path of ldms-perfdb-gen. Assume it is at the same --prefix as ldmsd */
	/* Remove my application name */
	c = strrchr(dir, '/');
	if (c)
		*c = 0;
	/* Remove my application directory */
	c = strrchr(dir, '/');
	if (c)
		*c = 0;
	char *perfdb = tempnam(NULL, NULL);
	if (!perfdb) {
		ovis_log(NULL, OVIS_LERROR,
			"tempnam() error: %d\n", errno);
		goto out;
	}
	rc = asprintf(&cmd, "%s/%s -o %s", dir, "bin/ldms-perfdb-gen -P", perfdb);
	ovis_log(NULL, OVIS_LERROR,
		"Generating perfdb using command: %s\n", cmd);
	rc = system(cmd);
	if (rc) {
		ovis_log(NULL, OVIS_LERROR,
			"system(\"%s\") error: %d\n", cmd, rc);
		goto out;
	}
	pf = fopen(perfdb, "r");
	if (!pf) {
		ovis_log(NULL, OVIS_LERROR,
			"Cannot open perfdb file '%s': %d\n", perfdb, errno);
		goto out;
	}
	jbuf = jbuf_new();
	while ((sz = getline(&line, &line_sz, pf)) > 0) {
		jbuf = jbuf_append_str(jbuf, line);
		if (!jbuf) {
			ovis_log(NULL, OVIS_LERROR,
				"%s:%d Memory allocation failure\n", __func__, __LINE__);
			goto out;
		}
	}
	/* out contains perfdb in json format */
	rc = json_parse_buffer(jbuf->buf, jbuf->cursor, &default_perfdb);
	if (rc) {
		ovis_log(NULL, OVIS_LERROR,
			"%s:%d perfdb parsing error: : %s\n",
			__func__, __LINE__,
			json_doc_errstr(default_perfdb));
		json_doc_free(default_perfdb);
		default_perfdb = NULL;
	}
out:
	free(jbuf);
	free(line);
	free(cmd);
	if (pf)
		fclose(pf);
	if (perfdb)
		unlink(perfdb);
	free(perfdb);
	return;
}

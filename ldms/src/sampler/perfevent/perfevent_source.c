/* -*- c-basic-offset: 8 -*-
 * Copyright (c) 2026 Open Grid Computing, Inc. All rights reserved.
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
 * \file perfevent_source.c
 * \brief Source-instance-first Linux perf event sampler.
 */

#define _GNU_SOURCE

#include "config.h"

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/limits.h>
#include <linux/perf_event.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "ovis_json/ovis_json.h"

#include "ldms.h"
#include "ldmsd.h"
#include "ldmsd_plug_api.h"
#include "../sampler_base.h"

#define SAMP "perfevent_source"

#define PE_SYSFS_PMU_ROOT "/sys/bus/event_source/devices"
#define PE_SYSFS_CPU_ROOT "/sys/devices/system/cpu"

/* LDMS string metrics include the terminating NUL byte. */
#define PE_LDMS_NAME_LEN   (NAME_MAX + 1)
#define PE_LDMS_UNIT_LEN   64
#define PE_LDMS_CPUS_LEN   256
#define PE_LDMS_LABEL_LEN  512

#define PE_SYSFS_TEXT_LEN 4096

#define _LOG(p, lvl, fmt, ...) ovis_log((p)->log, lvl, fmt, ## __VA_ARGS__)
#define _ERROR(p, fmt, ...) _LOG(p, OVIS_LERROR, fmt, ## __VA_ARGS__)
#define _WARN(p, fmt, ...) _LOG(p, OVIS_LWARN, fmt, ## __VA_ARGS__)
#define _DEBUG(p, fmt, ...) _LOG(p, OVIS_LDEBUG, fmt, ## __VA_ARGS__)

enum pe_inst_metric {
	PE_INST_NAME,
	PE_INST_PMU,
	PE_INST_INSTANCE_ID,
	PE_INST_BINDING_CPU,
	PE_INST_CPUS,
	PE_INST_LABELS,
	PE_INST_N
};

enum pe_event_metric {
	PE_EVENT_NAME,
	PE_EVENT_PMU,
	PE_EVENT_UNIT,
	PE_EVENT_SCALE,
	PE_EVENT_COUNTS,
	PE_EVENT_TIME_ENABLED,
	PE_EVENT_TIME_RUNNING,
	PE_EVENT_N
};

struct pe_format {
	char *name;
	int config;
	uint64_t mask;
};

struct pe_pmu {
	char *name;
	uint32_t type;
	struct pe_format *formats;
	size_t format_count;
	uint8_t *cpu_mask;
	size_t cpu_mask_len;
};

struct pe_instance {
	char *name;
	char *pmu_name;
	int instance_id;
	int binding_cpu;
	char *cpus;
	char *labels;
};

struct pe_event {
	char *name;
	char *pmu_name;
	char *unit;
	double scale;
	struct perf_event_attr attr;
	ldms_mval_t rec_mval;
};

struct pe_binding {
	struct pe_event *event;
	struct pe_instance *instance;
	int fd;
};

struct pe_read_value {
	uint64_t value;
	uint64_t time_enabled;
	uint64_t time_running;
};

struct pe_sampler {
	ldmsd_plug_handle_t plug;
	ovis_log_t log;
	base_data_t base;
	ldms_set_t set;
	json_doc_t config;

	uint8_t *online_cpu;
	size_t online_cpu_len;

	struct pe_pmu pmu;
	struct pe_instance *instances;
	size_t instance_count;
	struct pe_event *events;
	size_t event_count;
	struct pe_binding *bindings;
	size_t binding_count;

	int inst_recdef_mid;
	int event_recdef_mid;
	int instances_mid;
	int events_mid;
	int inst_metric_ids[PE_INST_N];
	int event_metric_ids[PE_EVENT_N];
};

static int perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu,
			   int group_fd, unsigned long flags)
{
	return syscall(SYS_perf_event_open, attr, pid, cpu, group_fd, flags);
}

static void rstrip(char *s)
{
	size_t len;

	if (!s)
		return;
	len = strlen(s);
	while (len && isspace((unsigned char)s[len - 1]))
		s[--len] = '\0';
}

static int xasprintf(char **out, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vasprintf(out, fmt, ap);
	va_end(ap);
	if (n < 0) {
		*out = NULL;
		return ENOMEM;
	}
	return 0;
}

static int component_valid(const char *s)
{
	if (!s || !s[0])
		return 0;
	if (strlen(s) > NAME_MAX)
		return 0;
	return !strchr(s, '/');
}

static int read_file_alloc(struct pe_sampler *p, const char *path, char **out,
			   size_t *len_out)
{
	int fd = -1;
	struct stat st;
	char *buf = NULL;
	size_t cap;
	ssize_t off = 0;
	ssize_t n;
	int rc = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		rc = errno;
		_ERROR(p, "open('%s') failed: %d\n", path, rc);
		goto out;
	}
	if (fstat(fd, &st)) {
		rc = errno;
		_ERROR(p, "fstat('%s') failed: %d\n", path, rc);
		goto out;
	}
	if (st.st_size < 0) {
		rc = EINVAL;
		goto out;
	}
	cap = (size_t)st.st_size + 1;
	if (cap < 2)
		cap = 2;
	buf = malloc(cap);
	if (!buf) {
		rc = ENOMEM;
		goto out;
	}
	while (1) {
		if ((size_t)off == cap - 1) {
			char *tmp;
			cap *= 2;
			tmp = realloc(buf, cap);
			if (!tmp) {
				rc = ENOMEM;
				goto out;
			}
			buf = tmp;
		}
		n = read(fd, buf + off, cap - 1 - (size_t)off);
		if (n < 0) {
			rc = errno;
			_ERROR(p, "read('%s') failed: %d\n", path, rc);
			goto out;
		}
		if (n == 0)
			break;
		off += n;
	}
	buf[off] = '\0';
	if (len_out)
		*len_out = (size_t)off;
	*out = buf;
	buf = NULL;
out:
	if (fd >= 0)
		close(fd);
	free(buf);
	return rc;
}

static int read_sysfs_text(struct pe_sampler *p, const char *path, char *buf,
			   size_t bufsz, int quiet_enoent)
{
	int fd = -1;
	size_t off = 0;
	ssize_t n;
	char extra;
	int rc = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		rc = errno;
		if (!(quiet_enoent && rc == ENOENT))
			_ERROR(p, "open('%s') failed: %d\n", path, rc);
		return rc;
	}
	while (off < bufsz - 1) {
		n = read(fd, buf + off, bufsz - 1 - off);
		if (n < 0) {
			rc = errno;
			_ERROR(p, "read('%s') failed: %d\n", path, rc);
			goto out;
		}
		if (n == 0)
			break;
		off += (size_t)n;
	}
	if (off == bufsz - 1) {
		n = read(fd, &extra, 1);
		if (n < 0) {
			rc = errno;
			_ERROR(p, "read('%s') failed: %d\n", path, rc);
			goto out;
		}
		if (n > 0) {
			rc = ENOBUFS;
			_ERROR(p, "'%s' is too large\n", path);
			goto out;
		}
	}
	buf[off] = '\0';
	rstrip(buf);
out:
	close(fd);
	return rc;
}

static int path_join3(char *buf, size_t bufsz, const char *a, const char *b,
		      const char *c)
{
	int n;

	n = snprintf(buf, bufsz, "%s/%s/%s", a, b, c);
	return (n < 0 || (size_t)n >= bufsz) ? ENAMETOOLONG : 0;
}

static int parse_u64_text(const char *s, uint64_t *out)
{
	char *end;
	uint64_t v;

	while (isspace((unsigned char)*s))
		s++;
	if (!isxdigit((unsigned char)*s))
		return EINVAL;
	errno = 0;
	v = strtoull(s, &end, 0);
	if (errno)
		return errno;
	while (isspace((unsigned char)*end))
		end++;
	if (*end)
		return EINVAL;
	*out = v;
	return 0;
}

static int parse_uint_file(struct pe_sampler *p, const char *path, uint32_t *out)
{
	char buf[PE_SYSFS_TEXT_LEN];
	uint64_t v;
	int rc;

	rc = read_sysfs_text(p, path, buf, sizeof(buf), 0);
	if (rc)
		return rc;
	rc = parse_u64_text(buf, &v);
	if (rc) {
		_ERROR(p, "bad integer in '%s': %s\n", path, buf);
		return rc;
	}
	if (v > UINT32_MAX) {
		_ERROR(p, "integer in '%s' is too large: %" PRIu64 "\n", path, v);
		return ERANGE;
	}
	*out = (uint32_t)v;
	return 0;
}

static void skip_separators(const char **sp)
{
	const char *s = *sp;

	while (*s && (isspace((unsigned char)*s) || *s == ','))
		s++;
	*sp = s;
}

static int parse_range_num(const char **sp, uint64_t *out)
{
	const char *s = *sp;
	char *end;

	if (!isdigit((unsigned char)*s))
		return EINVAL;
	errno = 0;
	*out = strtoull(s, &end, 10);
	if (errno)
		return errno;
	*sp = end;
	return 0;
}

static int parse_range_list(struct pe_sampler *p, const char *str, uint8_t *bits,
			    size_t bit_count, const char *what)
{
	const char *s = str;
	uint64_t a, b, v;
	int rc;
	int saw = 0;

	memset(bits, 0, bit_count);
	while (1) {
		skip_separators(&s);
		if (!*s)
			break;
		rc = parse_range_num(&s, &a);
		if (rc)
			goto bad;
		if (*s == '-') {
			s++;
			rc = parse_range_num(&s, &b);
			if (rc)
				goto bad;
		} else {
			b = a;
		}
		if (b < a)
			goto bad;
		if (b >= bit_count) {
			_ERROR(p, "%s CPU %" PRIu64 " is outside valid range 0-%zu\n",
			       what, b, bit_count ? bit_count - 1 : 0);
			return ERANGE;
		}
		for (v = a; v <= b; v++)
			bits[v] = 1;
		saw = 1;
		if (*s && !(isspace((unsigned char)*s) || *s == ','))
			goto bad;
	}
	if (!saw)
		goto bad;
	return 0;
bad:
	_ERROR(p, "bad %s range list: %s\n", what, str);
	return EINVAL;
}

static int range_list_max(struct pe_sampler *p, const char *str, uint64_t *max_out,
			  const char *what)
{
	const char *s = str;
	uint64_t a, b, max = 0;
	int rc;
	int saw = 0;

	while (1) {
		skip_separators(&s);
		if (!*s)
			break;
		rc = parse_range_num(&s, &a);
		if (rc)
			goto bad;
		if (*s == '-') {
			s++;
			rc = parse_range_num(&s, &b);
			if (rc)
				goto bad;
		} else {
			b = a;
		}
		if (b < a)
			goto bad;
		if (b > max)
			max = b;
		saw = 1;
		if (*s && !(isspace((unsigned char)*s) || *s == ','))
			goto bad;
	}
	if (!saw)
		goto bad;
	*max_out = max;
	return 0;
bad:
	_ERROR(p, "bad %s range list: %s\n", what, str);
	return EINVAL;
}

static int parse_bit_range_list(struct pe_sampler *p, const char *str,
				uint64_t *mask)
{
	const char *s = str;
	uint64_t a, b, v;
	int rc;
	int saw = 0;

	*mask = 0;
	while (1) {
		skip_separators(&s);
		if (!*s)
			break;
		rc = parse_range_num(&s, &a);
		if (rc)
			goto bad;
		if (*s == '-') {
			s++;
			rc = parse_range_num(&s, &b);
			if (rc)
				goto bad;
		} else {
			b = a;
		}
		if (b < a || b >= 64)
			goto bad;
		for (v = a; v <= b; v++)
			*mask |= UINT64_C(1) << v;
		saw = 1;
		if (*s && !(isspace((unsigned char)*s) || *s == ','))
			goto bad;
	}
	if (!saw)
		goto bad;
	return 0;
bad:
	_ERROR(p, "bad format bit range list: %s\n", str);
	return EINVAL;
}

static int json_check_keys(struct pe_sampler *p, json_entity_t e,
			   const char *path, const char * const *keys,
			   size_t key_count)
{
	json_entity_t attr;
	const char *name;
	size_t i;
	int found;

	for (attr = json_attr_first(e); attr; attr = json_attr_next(attr)) {
		name = json_attr_name(attr);
		found = 0;
		for (i = 0; i < key_count; i++) {
			if (0 == strcmp(name, keys[i])) {
				found = 1;
				break;
			}
		}
		if (!found) {
			_ERROR(p, "unsupported JSON field '%s' in %s\n", name, path);
			return EINVAL;
		}
	}
	return 0;
}

static int read_config_json(struct pe_sampler *p, const char *path)
{
	char *buf = NULL;
	size_t len = 0;
	int rc;

	rc = read_file_alloc(p, path, &buf, &len);
	if (rc)
		return rc;
	rc = json_parse_buffer(buf, len, &p->config);
	if (rc) {
		_ERROR(p, "JSON parse error in '%s': %s\n", path,
		       p->config ? json_doc_errstr(p->config) : "unknown error");
		if (p->config) {
			json_doc_free(p->config);
			p->config = NULL;
		}
		rc = EINVAL;
	}
	free(buf);
	return rc;
}

static void pe_close_bindings(struct pe_sampler *p)
{
	size_t i;

	for (i = 0; i < p->binding_count; i++) {
		if (p->bindings[i].fd >= 0) {
			close(p->bindings[i].fd);
			p->bindings[i].fd = -1;
		}
	}
}

static void pe_pmu_free(struct pe_pmu *pmu)
{
	size_t i;

	free(pmu->name);
	for (i = 0; i < pmu->format_count; i++)
		free(pmu->formats[i].name);
	free(pmu->formats);
	free(pmu->cpu_mask);
	memset(pmu, 0, sizeof(*pmu));
}

static void pe_free_config(struct pe_sampler *p)
{
	size_t i;

	pe_close_bindings(p);
	if (p->base)
		base_set_delete(p->base);
	p->set = NULL;
	if (p->base) {
		base_del(p->base);
		p->base = NULL;
	}
	if (p->config) {
		json_doc_free(p->config);
		p->config = NULL;
	}
	free(p->online_cpu);
	p->online_cpu = NULL;
	p->online_cpu_len = 0;
	pe_pmu_free(&p->pmu);
	for (i = 0; i < p->instance_count; i++) {
		free(p->instances[i].name);
		free(p->instances[i].pmu_name);
		free(p->instances[i].cpus);
		free(p->instances[i].labels);
	}
	free(p->instances);
	p->instances = NULL;
	p->instance_count = 0;
	for (i = 0; i < p->event_count; i++) {
		free(p->events[i].name);
		free(p->events[i].pmu_name);
		free(p->events[i].unit);
	}
	free(p->events);
	p->events = NULL;
	p->event_count = 0;
	free(p->bindings);
	p->bindings = NULL;
	p->binding_count = 0;
}

static int online_cpu_load(struct pe_sampler *p)
{
	char path[PATH_MAX];
	char buf[PE_SYSFS_TEXT_LEN];
	uint64_t max = 0;
	int rc;

	rc = snprintf(path, sizeof(path), "%s/online", PE_SYSFS_CPU_ROOT);
	if (rc < 0 || (size_t)rc >= sizeof(path))
		return ENAMETOOLONG;
	rc = read_sysfs_text(p, path, buf, sizeof(buf), 0);
	if (rc)
		return rc;

	rc = range_list_max(p, buf, &max, "online CPU");
	if (rc)
		return rc;
	if (max > SIZE_MAX - 1)
		return ERANGE;
	p->online_cpu_len = (size_t)max + 1;
	p->online_cpu = calloc(p->online_cpu_len, 1);
	if (!p->online_cpu)
		return ENOMEM;
	return parse_range_list(p, buf, p->online_cpu, p->online_cpu_len,
				"online CPU");
}

static int pmu_format_parse(struct pe_sampler *p, struct pe_format *format,
			    const char *text)
{
	const char *s = text;
	uint64_t config = 0;
	int rc;

	if (strncmp(s, "config", 6))
		goto bad;
	s += 6;
	if (isdigit((unsigned char)*s)) {
		rc = parse_range_num(&s, &config);
		if (rc)
			goto bad;
	}
	if (config > 3 || *s != ':')
		goto bad;
	s++;
	rc = parse_bit_range_list(p, s, &format->mask);
	if (rc)
		return rc;
	format->config = (int)config;
	return 0;
bad:
	_ERROR(p, "bad PMU format '%s'\n", text);
	return EINVAL;
}

static int pmu_format_append(struct pe_sampler *p, const char *name,
			     const char *text)
{
	struct pe_format *tmp;
	struct pe_format *fmt;
	int rc;

	tmp = realloc(p->pmu.formats,
		      (p->pmu.format_count + 1) * sizeof(*p->pmu.formats));
	if (!tmp)
		return ENOMEM;
	p->pmu.formats = tmp;
	fmt = &p->pmu.formats[p->pmu.format_count];
	memset(fmt, 0, sizeof(*fmt));
	fmt->name = strdup(name);
	if (!fmt->name)
		return ENOMEM;
	rc = pmu_format_parse(p, fmt, text);
	if (rc) {
		free(fmt->name);
		fmt->name = NULL;
		return rc;
	}
	p->pmu.format_count++;
	return 0;
}

static int pmu_load_formats(struct pe_sampler *p)
{
	char path[PATH_MAX];
	char fpath[PATH_MAX];
	char text[PE_SYSFS_TEXT_LEN];
	DIR *dir = NULL;
	struct dirent *dent;
	int rc;

	rc = path_join3(path, sizeof(path), PE_SYSFS_PMU_ROOT, p->pmu.name,
			"format");
	if (rc)
		return rc;
	dir = opendir(path);
	if (!dir) {
		rc = errno;
		_ERROR(p, "opendir('%s') failed: %d\n", path, rc);
		return rc;
	}
	while ((dent = readdir(dir))) {
		if (dent->d_name[0] == '.')
			continue;
		if (!component_valid(dent->d_name)) {
			rc = EINVAL;
			_ERROR(p, "bad PMU format component '%s'\n", dent->d_name);
			goto out;
		}
		rc = snprintf(fpath, sizeof(fpath), "%s/%s", path, dent->d_name);
		if (rc < 0 || (size_t)rc >= sizeof(fpath)) {
			rc = ENAMETOOLONG;
			goto out;
		}
		rc = read_sysfs_text(p, fpath, text, sizeof(text), 0);
		if (rc)
			goto out;
		rc = pmu_format_append(p, dent->d_name, text);
		if (rc)
			goto out;
	}
	if (!p->pmu.format_count) {
		_ERROR(p, "PMU '%s' has no format fields\n", p->pmu.name);
		rc = ENOENT;
	}
out:
	closedir(dir);
	return rc;
}

static int pmu_load_cpumask(struct pe_sampler *p)
{
	char path[PATH_MAX];
	char buf[PE_SYSFS_TEXT_LEN];
	int rc;
	size_t i;

	p->pmu.cpu_mask_len = p->online_cpu_len;
	p->pmu.cpu_mask = calloc(p->pmu.cpu_mask_len, 1);
	if (!p->pmu.cpu_mask)
		return ENOMEM;

	if (0 == strcmp(p->pmu.name, "cpu")) {
		for (i = 0; i < p->online_cpu_len; i++)
			p->pmu.cpu_mask[i] = p->online_cpu[i];
		return 0;
	}

	rc = path_join3(path, sizeof(path), PE_SYSFS_PMU_ROOT, p->pmu.name,
			"cpumask");
	if (rc)
		return rc;
	rc = read_sysfs_text(p, path, buf, sizeof(buf), 0);
	if (rc)
		return rc;
	rc = parse_range_list(p, buf, p->pmu.cpu_mask, p->pmu.cpu_mask_len,
			      "PMU cpumask");
	if (rc)
		return rc;
	for (i = 0; i < p->pmu.cpu_mask_len; i++) {
		if (p->pmu.cpu_mask[i] && !p->online_cpu[i]) {
			_ERROR(p, "PMU '%s' cpumask CPU %zu is not online\n",
			       p->pmu.name, i);
			return EINVAL;
		}
	}
	return 0;
}

static int pmu_load(struct pe_sampler *p, const char *pmu_name)
{
	char path[PATH_MAX];
	int rc;

	if (!component_valid(pmu_name)) {
		_ERROR(p, "bad PMU name '%s'\n", pmu_name ? pmu_name : "(null)");
		return EINVAL;
	}
	p->pmu.name = strdup(pmu_name);
	if (!p->pmu.name)
		return ENOMEM;
	rc = path_join3(path, sizeof(path), PE_SYSFS_PMU_ROOT, p->pmu.name,
			"type");
	if (rc)
		return rc;
	rc = parse_uint_file(p, path, &p->pmu.type);
	if (rc)
		return rc;
	rc = pmu_load_formats(p);
	if (rc)
		return rc;
	return pmu_load_cpumask(p);
}

static struct pe_format *pmu_format_find(struct pe_sampler *p, const char *name)
{
	size_t i;

	for (i = 0; i < p->pmu.format_count; i++) {
		if (0 == strcmp(p->pmu.formats[i].name, name))
			return &p->pmu.formats[i];
	}
	return NULL;
}

static int mask_bit_count(uint64_t mask)
{
	int n = 0;

	while (mask) {
		n += mask & 1;
		mask >>= 1;
	}
	return n;
}

static void attr_format_value_set(uint64_t mask, __u64 *field, uint64_t value)
{
	uint64_t f;
	uint64_t v;

	for (f = 1, v = 1; f; f <<= 1) {
		if (!(mask & f))
			continue;
		if (value & v)
			*field |= f;
		else
			*field &= ~f;
		v <<= 1;
	}
}

static int event_encode_term(struct pe_sampler *p, struct pe_event *event,
			     const char *term, uint64_t value)
{
	struct pe_format *fmt;
	__u64 *field;
	int bits;

	fmt = pmu_format_find(p, term);
	if (!fmt) {
		_ERROR(p, "raw/sysfs field '%s' is not defined by PMU '%s'\n",
		       term, p->pmu.name);
		return ENOENT;
	}
	bits = mask_bit_count(fmt->mask);
	if (bits < 64 && (value >> bits)) {
		_ERROR(p, "value 0x%" PRIx64 " is too wide for field '%s'\n",
		       value, term);
		return ERANGE;
	}
	switch (fmt->config) {
	case 0:
		field = &event->attr.config;
		break;
	case 1:
		field = &event->attr.config1;
		break;
#ifdef PERF_ATTR_SIZE_VER1
	case 2:
		field = &event->attr.config2;
		break;
#endif
#ifdef PERF_ATTR_SIZE_VER8
	case 3:
		field = &event->attr.config3;
		break;
#endif
	default:
		_ERROR(p, "unsupported perf_event_attr config%d for field '%s'\n",
		       fmt->config, term);
		return ENOTSUP;
	}
	attr_format_value_set(fmt->mask, field, value);
	return 0;
}

static int event_init_attr(struct pe_sampler *p, struct pe_event *event)
{
	memset(&event->attr, 0, sizeof(event->attr));
	event->attr.type = p->pmu.type;
	event->attr.size = sizeof(event->attr);
	event->attr.disabled = 1;
	event->attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED |
				  PERF_FORMAT_TOTAL_TIME_RUNNING;
	return 0;
}

static int event_encode_terms_text(struct pe_sampler *p, struct pe_event *event,
				   const char *text)
{
	const char *s = text;
	char term[NAME_MAX + 1];
	size_t len;
	uint64_t value;
	char *end;
	int rc;
	int count = 0;

	while (1) {
		skip_separators(&s);
		if (!*s)
			break;
		len = strcspn(s, "=");
		if (!len || len > NAME_MAX || s[len] != '=')
			goto bad;
		memcpy(term, s, len);
		term[len] = '\0';
		if (!component_valid(term))
			goto bad;
		s += len + 1;
		if (!isxdigit((unsigned char)*s))
			goto bad;
		errno = 0;
		value = strtoull(s, &end, 0);
		if (errno)
			return errno;
		s = end;
		rc = event_encode_term(p, event, term, value);
		if (rc)
			return rc;
		count++;
		if (*s && !(isspace((unsigned char)*s) || *s == ','))
			goto bad;
	}
	if (!count)
		goto bad;
	return 0;
bad:
	_ERROR(p, "bad event term list for '%s': %s\n", event->name, text);
	return EINVAL;
}

static int event_parse_raw_value(struct pe_sampler *p, json_entity_t value,
				 const char *field, uint64_t *out)
{
	enum json_value_e type;
	int64_t i;
	int rc;

	type = json_entity_type(value);
	switch (type) {
	case JSON_INT_VALUE:
		i = json_value_int(value);
		if (i < 0) {
			_ERROR(p, "raw field '%s' must be non-negative\n", field);
			return EINVAL;
		}
		*out = (uint64_t)i;
		return 0;
	case JSON_STRING_VALUE:
		rc = parse_u64_text(json_value_cstr(value), out);
		if (rc)
			_ERROR(p, "raw field '%s' has bad value '%s'\n",
			       field, json_value_cstr(value));
		return rc;
	default:
		_ERROR(p, "raw field '%s' must be a string or integer, got %s\n",
		       field, json_type_name(type));
		return EINVAL;
	}
}

static int event_encode_raw(struct pe_sampler *p, struct pe_event *event,
			    json_entity_t raw)
{
	json_entity_t attr;
	const char *field;
	uint64_t value;
	int count = 0;
	int rc;

	if (json_entity_type(raw) != JSON_DICT_VALUE) {
		_ERROR(p, "event '%s' raw must be an object\n", event->name);
		return EINVAL;
	}
	for (attr = json_attr_first(raw); attr; attr = json_attr_next(attr)) {
		field = json_attr_name(attr);
		if (!component_valid(field)) {
			_ERROR(p, "bad raw field name '%s'\n", field);
			return EINVAL;
		}
		rc = event_parse_raw_value(p, json_attr_value(attr), field, &value);
		if (rc)
			return rc;
		rc = event_encode_term(p, event, field, value);
		if (rc)
			return rc;
		count++;
	}
	if (!count) {
		_ERROR(p, "event '%s' raw object is empty\n", event->name);
		return EINVAL;
	}
	return 0;
}

static int event_read_metadata(struct pe_sampler *p, struct pe_event *event)
{
	char path[PATH_MAX];
	char text[PE_SYSFS_TEXT_LEN];
	int n;
	int rc;

	event->scale = 1.0;
	event->unit = strdup("");
	if (!event->unit)
		return ENOMEM;

	n = snprintf(path, sizeof(path), "%s/%s/events/%s.scale",
		     PE_SYSFS_PMU_ROOT, p->pmu.name, event->name);
	if (n < 0 || (size_t)n >= sizeof(path))
		return ENAMETOOLONG;
	rc = read_sysfs_text(p, path, text, sizeof(text), 1);
	if (!rc) {
		errno = 0;
		event->scale = strtod(text, NULL);
		if (errno) {
			_ERROR(p, "bad scale in '%s': %s\n", path, text);
			return errno;
		}
	} else if (rc != ENOENT) {
		return rc;
	}

	n = snprintf(path, sizeof(path), "%s/%s/events/%s.unit",
		     PE_SYSFS_PMU_ROOT, p->pmu.name, event->name);
	if (n < 0 || (size_t)n >= sizeof(path))
		return ENAMETOOLONG;
	rc = read_sysfs_text(p, path, text, sizeof(text), 1);
	if (!rc) {
		free(event->unit);
		event->unit = strdup(text);
		if (!event->unit)
			return ENOMEM;
	} else if (rc != ENOENT) {
		return rc;
	}
	return 0;
}

static int event_resolve_sysfs(struct pe_sampler *p, struct pe_event *event)
{
	char path[PATH_MAX];
	char text[PE_SYSFS_TEXT_LEN];
	int n;
	int rc;

	n = snprintf(path, sizeof(path), "%s/%s/events/%s",
		     PE_SYSFS_PMU_ROOT, p->pmu.name, event->name);
	if (n < 0 || (size_t)n >= sizeof(path))
		return ENAMETOOLONG;
	rc = read_sysfs_text(p, path, text, sizeof(text), 0);
	if (rc) {
		_ERROR(p, "event '%s' does not resolve on PMU '%s'\n",
		       event->name, p->pmu.name);
		return rc;
	}
	return event_encode_terms_text(p, event, text);
}

static int read_cpu_text(struct pe_sampler *p, int cpu, const char *suffix,
			 char *buf, size_t bufsz, int quiet_enoent)
{
	char path[PATH_MAX];
	int n;

	n = snprintf(path, sizeof(path), "%s/cpu%d/%s", PE_SYSFS_CPU_ROOT,
		     cpu, suffix);
	if (n < 0 || (size_t)n >= sizeof(path))
		return ENAMETOOLONG;
	return read_sysfs_text(p, path, buf, bufsz, quiet_enoent);
}

static int read_cpu_numa_node(int cpu)
{
	char path[PATH_MAX];
	DIR *dir;
	struct dirent *dent;
	int node = -1;
	int n;

	n = snprintf(path, sizeof(path), "%s/cpu%d", PE_SYSFS_CPU_ROOT, cpu);
	if (n < 0 || (size_t)n >= sizeof(path))
		return -1;
	dir = opendir(path);
	if (!dir)
		return -1;
	while ((dent = readdir(dir))) {
		if (sscanf(dent->d_name, "node%d", &node) == 1)
			break;
	}
	closedir(dir);
	return node;
}

static char *labels_for_cpu(struct pe_sampler *p, int cpu)
{
	char pkg[64] = "";
	char l3[PE_LDMS_CPUS_LEN] = "";
	char label[PE_LDMS_LABEL_LEN];
	int node;
	int have_pkg = 0;
	int have_l3 = 0;
	int off = 0;
	int n;

	if (0 == read_cpu_text(p, cpu, "topology/physical_package_id",
			       pkg, sizeof(pkg), 1))
		have_pkg = 1;
	if (0 == read_cpu_text(p, cpu, "cache/index3/shared_cpu_list",
			       l3, sizeof(l3), 1))
		have_l3 = 1;
	node = read_cpu_numa_node(cpu);

	label[0] = '\0';
	if (have_pkg) {
		n = snprintf(label + off, sizeof(label) - off,
			     "package_id=%s", pkg);
		if (n > 0)
			off += n < (int)(sizeof(label) - off) ? n : 0;
	}
	if (node >= 0 && off < (int)sizeof(label)) {
		n = snprintf(label + off, sizeof(label) - off,
			     "%snuma_node_id=%d", off ? "," : "", node);
		if (n > 0)
			off += n < (int)(sizeof(label) - off) ? n : 0;
	}
	if (have_l3 && off < (int)sizeof(label)) {
		n = snprintf(label + off, sizeof(label) - off,
			     "%sl3_cpus=%s", off ? "," : "", l3);
		if (n > 0)
			off += n < (int)(sizeof(label) - off) ? n : 0;
	}
	return strdup(label);
}

static int instance_append(struct pe_sampler *p, int cpu)
{
	struct pe_instance *tmp;
	struct pe_instance *inst;
	int rc;

	tmp = realloc(p->instances,
		      (p->instance_count + 1) * sizeof(*p->instances));
	if (!tmp)
		return ENOMEM;
	p->instances = tmp;
	inst = &p->instances[p->instance_count];
	memset(inst, 0, sizeof(*inst));
	inst->instance_id = (int)p->instance_count;
	inst->binding_cpu = cpu;
	inst->pmu_name = strdup(p->pmu.name);
	if (!inst->pmu_name) {
		rc = ENOMEM;
		goto err;
	}
	if (0 == strcmp(p->pmu.name, "cpu"))
		rc = xasprintf(&inst->name, "cpu:%d", cpu);
	else
		rc = xasprintf(&inst->name, "%s:cpu%d", p->pmu.name, cpu);
	if (rc)
		goto err;
	rc = xasprintf(&inst->cpus, "%d", cpu);
	if (rc)
		goto err;
	inst->labels = labels_for_cpu(p, cpu);
	if (!inst->labels) {
		rc = ENOMEM;
		goto err;
	}
	p->instance_count++;
	return 0;
err:
	free(inst->name);
	free(inst->pmu_name);
	free(inst->cpus);
	free(inst->labels);
	memset(inst, 0, sizeof(*inst));
	return rc;
}

static int instances_build(struct pe_sampler *p, json_entity_t source)
{
	json_entity_t cpus;
	uint8_t *filter = NULL;
	const char *cpu_filter = NULL;
	size_t i;
	int rc;

	cpus = json_value_find(source, "cpus");
	if (cpus) {
		if (json_entity_type(cpus) != JSON_STRING_VALUE) {
			_ERROR(p, "source.cpus must be a string\n");
			return EINVAL;
		}
		cpu_filter = json_value_cstr(cpus);
		filter = calloc(p->online_cpu_len, 1);
		if (!filter)
			return ENOMEM;
		rc = parse_range_list(p, cpu_filter, filter, p->online_cpu_len,
				      "source.cpus");
		if (rc)
			goto out;
		for (i = 0; i < p->online_cpu_len; i++) {
			if (filter[i] && !p->online_cpu[i]) {
				_ERROR(p, "source.cpus CPU %zu is not online\n", i);
				rc = EINVAL;
				goto out;
			}
		}
	}

	for (i = 0; i < p->pmu.cpu_mask_len; i++) {
		if (!p->pmu.cpu_mask[i])
			continue;
		if (filter && !filter[i])
			continue;
		rc = instance_append(p, (int)i);
		if (rc)
			goto out;
	}
	if (!p->instance_count) {
		_ERROR(p, "PMU '%s' has no valid binding CPU after filters\n",
		       p->pmu.name);
		rc = ENOENT;
		goto out;
	}
	rc = 0;
out:
	free(filter);
	return rc;
}

static int event_name_duplicate(struct pe_sampler *p, const char *name)
{
	size_t i;

	for (i = 0; i < p->event_count; i++) {
		if (0 == strcmp(p->events[i].name, name))
			return 1;
	}
	return 0;
}

static int event_append_from_json(struct pe_sampler *p, json_entity_t e)
{
	static const char * const event_keys[] = { "name", "raw" };
	json_entity_t name_e;
	json_entity_t raw_e;
	struct pe_event *tmp;
	struct pe_event *event;
	const char *name;
	int rc;

	if (json_entity_type(e) != JSON_DICT_VALUE) {
		_ERROR(p, "each events[] entry must be an object\n");
		return EINVAL;
	}
	rc = json_check_keys(p, e, "events[]", event_keys,
			     sizeof(event_keys) / sizeof(event_keys[0]));
	if (rc)
		return rc;
	name_e = json_value_find(e, "name");
	if (!name_e || json_entity_type(name_e) != JSON_STRING_VALUE) {
		_ERROR(p, "events[] entry requires string field 'name'\n");
		return EINVAL;
	}
	name = json_value_cstr(name_e);
	if (!component_valid(name)) {
		_ERROR(p, "bad event name '%s'\n", name ? name : "(null)");
		return EINVAL;
	}
	if (event_name_duplicate(p, name)) {
		_ERROR(p, "duplicate event name '%s'\n", name);
		return EINVAL;
	}

	tmp = realloc(p->events, (p->event_count + 1) * sizeof(*p->events));
	if (!tmp)
		return ENOMEM;
	p->events = tmp;
	event = &p->events[p->event_count];
	memset(event, 0, sizeof(*event));
	event->name = strdup(name);
	event->pmu_name = strdup(p->pmu.name);
	if (!event->name || !event->pmu_name) {
		rc = ENOMEM;
		goto err;
	}
	event_init_attr(p, event);

	raw_e = json_value_find(e, "raw");
	if (raw_e)
		rc = event_encode_raw(p, event, raw_e);
	else
		rc = event_resolve_sysfs(p, event);
	if (rc)
		goto err;
	rc = event_read_metadata(p, event);
	if (rc)
		goto err;
	p->event_count++;
	return 0;
err:
	free(event->name);
	free(event->pmu_name);
	free(event->unit);
	memset(event, 0, sizeof(*event));
	return rc;
}

static int events_build(struct pe_sampler *p, json_entity_t root)
{
	json_entity_t events;
	json_entity_t e;
	int rc;

	events = json_value_find(root, "events");
	if (!events || json_entity_type(events) != JSON_LIST_VALUE) {
		_ERROR(p, "top-level 'events' must be a non-empty list\n");
		return EINVAL;
	}
	for (e = json_item_first(events); e; e = json_item_next(e)) {
		rc = event_append_from_json(p, e);
		if (rc)
			return rc;
	}
	if (!p->event_count) {
		_ERROR(p, "top-level 'events' must be a non-empty list\n");
		return EINVAL;
	}
	return 0;
}

static int bindings_build(struct pe_sampler *p)
{
	size_t i, j, k = 0;

	p->binding_count = p->event_count * p->instance_count;
	p->bindings = calloc(p->binding_count, sizeof(*p->bindings));
	if (!p->bindings)
		return ENOMEM;
	for (i = 0; i < p->event_count; i++) {
		for (j = 0; j < p->instance_count; j++) {
			p->bindings[k].event = &p->events[i];
			p->bindings[k].instance = &p->instances[j];
			p->bindings[k].fd = -1;
			k++;
		}
	}
	return 0;
}

static int bindings_open(struct pe_sampler *p)
{
	size_t i;
	struct pe_binding *b;
	struct perf_event_attr attr;

	for (i = 0; i < p->binding_count; i++) {
		b = &p->bindings[i];
		attr = b->event->attr;
		b->fd = perf_event_open(&attr, -1, b->instance->binding_cpu,
					-1, 0);
		if (b->fd < 0) {
			int rc = errno;
			_ERROR(p, "perf_event_open failed: pmu=%s event=%s "
			       "instance=%s binding_cpu=%d errno=%d\n",
			       b->event->pmu_name, b->event->name,
			       b->instance->name, b->instance->binding_cpu, rc);
			pe_close_bindings(p);
			return rc;
		}
	}
	return 0;
}

static int bindings_start(struct pe_sampler *p)
{
	size_t i;
	int rc;

	for (i = 0; i < p->binding_count; i++) {
		rc = ioctl(p->bindings[i].fd, PERF_EVENT_IOC_RESET, 0);
		if (rc) {
			rc = errno;
			_ERROR(p, "PERF_EVENT_IOC_RESET failed for event=%s "
			       "instance=%s errno=%d\n",
			       p->bindings[i].event->name,
			       p->bindings[i].instance->name, rc);
			return rc;
		}
		rc = ioctl(p->bindings[i].fd, PERF_EVENT_IOC_ENABLE, 0);
		if (rc) {
			rc = errno;
			_ERROR(p, "PERF_EVENT_IOC_ENABLE failed for event=%s "
			       "instance=%s errno=%d\n",
			       p->bindings[i].event->name,
			       p->bindings[i].instance->name, rc);
			return rc;
		}
	}
	return 0;
}

static int make_set(struct pe_sampler *p)
{
	struct ldms_metric_template_s inst_tmp[] = {
		{ .name = "name", .type = LDMS_V_CHAR_ARRAY,
		  .len = PE_LDMS_NAME_LEN },
		{ .name = "pmu", .type = LDMS_V_CHAR_ARRAY,
		  .len = PE_LDMS_NAME_LEN },
		{ .name = "instance_id", .type = LDMS_V_U32, .len = 1 },
		{ .name = "binding_cpu", .type = LDMS_V_S32, .len = 1 },
		{ .name = "cpus", .type = LDMS_V_CHAR_ARRAY,
		  .len = PE_LDMS_CPUS_LEN },
		{ .name = "labels", .type = LDMS_V_CHAR_ARRAY,
		  .len = PE_LDMS_LABEL_LEN },
		{ 0 }
	};
	struct ldms_metric_template_s event_tmp[] = {
		{ .name = "name", .type = LDMS_V_CHAR_ARRAY,
		  .len = PE_LDMS_NAME_LEN },
		{ .name = "pmu", .type = LDMS_V_CHAR_ARRAY,
		  .len = PE_LDMS_NAME_LEN },
		{ .name = "unit", .type = LDMS_V_CHAR_ARRAY,
		  .len = PE_LDMS_UNIT_LEN },
		{ .name = "scale", .type = LDMS_V_D64, .len = 1 },
		{ .name = "counts", .type = LDMS_V_U64_ARRAY,
		  .len = p->instance_count },
		{ .name = "time_enabled", .type = LDMS_V_U64_ARRAY,
		  .len = p->instance_count },
		{ .name = "time_running", .type = LDMS_V_U64_ARRAY,
		  .len = p->instance_count },
		{ 0 }
	};
	ldms_schema_t schema;
	ldms_record_t inst_recdef = NULL;
	ldms_record_t event_recdef = NULL;
	int inst_recdef_added = 0;
	int event_recdef_added = 0;
	ldms_mval_t rec;
	ldms_mval_t list;
	size_t heap_sz;
	size_t i, j;
	int rc;
	int in_txn = 0;

	schema = base_schema_new(p->base);
	if (!schema)
		return errno;

	inst_recdef = ldms_record_from_template("perfevent_source_instance",
						inst_tmp, p->inst_metric_ids);
	if (!inst_recdef) {
		rc = errno;
		goto err;
	}
	rc = ldms_schema_record_add(schema, inst_recdef);
	if (rc < 0) {
		rc = -rc;
		goto err;
	}
	p->inst_recdef_mid = rc;
	inst_recdef_added = 1;

	event_recdef = ldms_record_from_template("perfevent_source_event",
						 event_tmp, p->event_metric_ids);
	if (!event_recdef) {
		rc = errno;
		goto err;
	}
	rc = ldms_schema_record_add(schema, event_recdef);
	if (rc < 0) {
		rc = -rc;
		goto err;
	}
	p->event_recdef_mid = rc;
	event_recdef_added = 1;

	heap_sz = p->instance_count * ldms_record_heap_size_get(inst_recdef);
	rc = ldms_schema_metric_list_add(schema, "instances", NULL, heap_sz);
	if (rc < 0) {
		rc = -rc;
		goto err;
	}
	p->instances_mid = rc;

	heap_sz = p->event_count * ldms_record_heap_size_get(event_recdef);
	rc = ldms_schema_metric_list_add(schema, "events", NULL, heap_sz);
	if (rc < 0) {
		rc = -rc;
		goto err;
	}
	p->events_mid = rc;

	p->set = base_set_new(p->base);
	if (!p->set) {
		rc = errno;
		goto err;
	}

	ldms_transaction_begin(p->set);
	in_txn = 1;

	list = ldms_metric_get(p->set, p->instances_mid);
	for (i = 0; i < p->instance_count; i++) {
		rec = ldms_record_alloc(p->set, p->inst_recdef_mid);
		if (!rec) {
			rc = errno;
			goto err;
		}
		rc = ldms_list_append_record(p->set, list, rec);
		if (rc)
			goto err;
		ldms_record_array_set_str(rec, p->inst_metric_ids[PE_INST_NAME],
					  p->instances[i].name);
		ldms_record_array_set_str(rec, p->inst_metric_ids[PE_INST_PMU],
					  p->instances[i].pmu_name);
		ldms_record_set_u32(rec, p->inst_metric_ids[PE_INST_INSTANCE_ID],
				    p->instances[i].instance_id);
		ldms_record_set_s32(rec, p->inst_metric_ids[PE_INST_BINDING_CPU],
				    p->instances[i].binding_cpu);
		ldms_record_array_set_str(rec, p->inst_metric_ids[PE_INST_CPUS],
					  p->instances[i].cpus);
		ldms_record_array_set_str(rec, p->inst_metric_ids[PE_INST_LABELS],
					  p->instances[i].labels);
	}

	list = ldms_metric_get(p->set, p->events_mid);
	for (i = 0; i < p->event_count; i++) {
		rec = ldms_record_alloc(p->set, p->event_recdef_mid);
		if (!rec) {
			rc = errno;
			goto err;
		}
		rc = ldms_list_append_record(p->set, list, rec);
		if (rc)
			goto err;
		p->events[i].rec_mval = rec;
		ldms_record_array_set_str(rec, p->event_metric_ids[PE_EVENT_NAME],
					  p->events[i].name);
		ldms_record_array_set_str(rec, p->event_metric_ids[PE_EVENT_PMU],
					  p->events[i].pmu_name);
		ldms_record_array_set_str(rec, p->event_metric_ids[PE_EVENT_UNIT],
					  p->events[i].unit);
		ldms_record_set_double(rec, p->event_metric_ids[PE_EVENT_SCALE],
				       p->events[i].scale);
		for (j = 0; j < p->instance_count; j++) {
			ldms_record_array_set_u64(rec,
				p->event_metric_ids[PE_EVENT_COUNTS], j, 0);
			ldms_record_array_set_u64(rec,
				p->event_metric_ids[PE_EVENT_TIME_ENABLED], j, 0);
			ldms_record_array_set_u64(rec,
				p->event_metric_ids[PE_EVENT_TIME_RUNNING], j, 0);
		}
	}

	ldms_transaction_end(p->set);
	return 0;
err:
	if (in_txn)
		ldms_transaction_end(p->set);
	if (!inst_recdef_added && inst_recdef)
		ldms_record_delete(inst_recdef);
	if (!event_recdef_added && event_recdef)
		ldms_record_delete(event_recdef);
	if (p->base)
		base_set_delete(p->base);
	p->set = NULL;
	return rc;
}

static int config_parse_and_plan(struct pe_sampler *p)
{
	static const char * const root_keys[] = { "source", "events" };
	static const char * const source_keys[] = { "pmu", "cpus" };
	json_entity_t root;
	json_entity_t source;
	json_entity_t pmu;
	const char *pmu_name;
	int rc;

	root = json_doc_root(p->config);
	if (json_entity_type(root) != JSON_DICT_VALUE) {
		_ERROR(p, "JSON root must be an object\n");
		return EINVAL;
	}
	rc = json_check_keys(p, root, "root", root_keys,
			     sizeof(root_keys) / sizeof(root_keys[0]));
	if (rc)
		return rc;
	source = json_value_find(root, "source");
	if (!source || json_entity_type(source) != JSON_DICT_VALUE) {
		_ERROR(p, "top-level 'source' must be an object\n");
		return EINVAL;
	}
	rc = json_check_keys(p, source, "source", source_keys,
			     sizeof(source_keys) / sizeof(source_keys[0]));
	if (rc)
		return rc;
	pmu = json_value_find(source, "pmu");
	if (!pmu || json_entity_type(pmu) != JSON_STRING_VALUE) {
		_ERROR(p, "source.pmu must be a string\n");
		return EINVAL;
	}
	pmu_name = json_value_cstr(pmu);
	if (!component_valid(pmu_name)) {
		_ERROR(p, "bad source.pmu '%s'\n", pmu_name ? pmu_name : "(null)");
		return EINVAL;
	}
	rc = online_cpu_load(p);
	if (rc)
		return rc;
	rc = pmu_load(p, pmu_name);
	if (rc)
		return rc;
	rc = instances_build(p, source);
	if (rc)
		return rc;
	rc = events_build(p, root);
	if (rc)
		return rc;
	return bindings_build(p);
}

static const char *usage(ldmsd_plug_handle_t handle)
{
	(void)handle;
	return "config name=<plugin_instance> " BASE_CONFIG_USAGE
	       " conf=<json_file>\n"
	       "    conf       JSON configuration file with source.pmu and events.\n";
}

static int config(ldmsd_plug_handle_t handle, struct attr_value_list *kwl,
		  struct attr_value_list *avl)
{
	struct pe_sampler *p = ldmsd_plug_ctxt_get(handle);
	const char *conf;
	int rc;

	(void)kwl;
	if (!p)
		return ENOENT;
	if (p->base) {
		_ERROR(p, "sampler is already configured\n");
		return EBUSY;
	}
	conf = av_value(avl, "conf");
	if (!conf || !conf[0]) {
		_ERROR(p, "'conf' parameter is required\n");
		return EINVAL;
	}

	p->base = base_config(avl, ldmsd_plug_cfg_name_get(handle), SAMP, p->log);
	if (!p->base)
		return errno;
	p->base->mylog = p->log;

	rc = read_config_json(p, conf);
	if (rc)
		goto err;
	rc = config_parse_and_plan(p);
	if (rc)
		goto err;
	rc = bindings_open(p);
	if (rc)
		goto err;
	rc = make_set(p);
	if (rc)
		goto err;
	rc = bindings_start(p);
	if (rc)
		goto err;
	return 0;
err:
	pe_free_config(p);
	return rc;
}

static int sample(ldmsd_plug_handle_t handle)
{
	struct pe_sampler *p = ldmsd_plug_ctxt_get(handle);
	struct pe_binding *b;
	struct pe_read_value rv;
	ssize_t n;
	size_t i;
	int inst_id;
	ldms_mval_t rec;

	if (!p || !p->set)
		return ENOENT;
	base_sample_begin(p->base);
	for (i = 0; i < p->binding_count; i++) {
		b = &p->bindings[i];
		n = read(b->fd, &rv, sizeof(rv));
		if (n < 0) {
			_ERROR(p, "read failed: event=%s instance=%s errno=%d\n",
			       b->event->name, b->instance->name, errno);
			continue;
		}
		if (n != sizeof(rv)) {
			_ERROR(p, "short read: event=%s instance=%s bytes=%zd\n",
			       b->event->name, b->instance->name, n);
			continue;
		}
		rec = b->event->rec_mval;
		inst_id = b->instance->instance_id;
		ldms_record_array_set_u64(rec,
			p->event_metric_ids[PE_EVENT_COUNTS], inst_id, rv.value);
		ldms_record_array_set_u64(rec,
			p->event_metric_ids[PE_EVENT_TIME_ENABLED], inst_id,
			rv.time_enabled);
		ldms_record_array_set_u64(rec,
			p->event_metric_ids[PE_EVENT_TIME_RUNNING], inst_id,
			rv.time_running);
	}
	base_sample_end(p->base);
	return 0;
}

static int constructor(ldmsd_plug_handle_t handle)
{
	struct pe_sampler *p;

	p = calloc(1, sizeof(*p));
	if (!p)
		return ENOMEM;
	p->plug = handle;
	p->log = ldmsd_plug_log_get(handle);
	p->inst_recdef_mid = -1;
	p->event_recdef_mid = -1;
	p->instances_mid = -1;
	p->events_mid = -1;
	ldmsd_plug_ctxt_set(handle, p);
	return 0;
}

static void destructor(ldmsd_plug_handle_t handle)
{
	struct pe_sampler *p = ldmsd_plug_ctxt_get(handle);

	if (!p)
		return;
	pe_free_config(p);
	free(p);
	ldmsd_plug_ctxt_set(handle, NULL);
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

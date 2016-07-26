/*****************************************************************************\
 *  node_features_knl_cray.c - Plugin for managing Cray KNL state information
 *****************************************************************************
 *  Copyright (C) 2016 SchedMD LLC.
 *  Written by Morris Jette <jette@schedmd.com>
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include "config.h"

#define _GNU_SOURCE	/* For POLLRDHUP */
#include <ctype.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if HAVE_JSON_C_INC
#  include <json-c/json.h>
#elif HAVE_JSON_INC
#  include <json/json.h>
#endif

#include "slurm/slurm.h"

#include "src/common/assoc_mgr.h"
#include "src/common/bitstring.h"
#include "src/common/fd.h"
#include "src/common/gres.h"
#include "src/common/list.h"
#include "src/common/macros.h"
#include "src/common/pack.h"
#include "src/common/parse_config.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/timers.h"
#include "src/common/uid.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/slurmctld/job_scheduler.h"
#include "src/slurmctld/locks.h"
#include "src/slurmctld/node_scheduler.h"
#include "src/slurmctld/reservation.h"
#include "src/slurmctld/slurmctld.h"
#include "src/slurmctld/state_save.h"

/* Maximum poll wait time for child processes, in milliseconds */
#define MAX_POLL_WAIT 500

/* Intel Knights Landing Configuration Modes */
#define KNL_NUMA_CNT	5
#define KNL_MCDRAM_CNT	4
#define KNL_NUMA_FLAG	0x00ff
#define KNL_ALL2ALL	0x0001
#define KNL_SNC2	0x0002
#define KNL_SNC4	0x0004
#define KNL_HEMI	0x0008
#define KNL_QUAD	0x0010
#define KNL_MCDRAM_FLAG	0xff00
#define KNL_CACHE	0x0100
#define KNL_EQUAL	0x0200
#define KNL_SPLIT	0x0400
#define KNL_FLAT	0x0800

/* These are defined here so when we link with something other than
 * the slurmctld we will have these symbols defined.  They will get
 * overwritten when linking with the slurmctld.
 */
#if defined (__APPLE__)
slurmctld_config_t slurmctld_config __attribute__((weak_import));
#else
slurmctld_config_t slurmctld_config;
#endif

/*
 * These variables are required by the burst buffer plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  SLURM uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *      <application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "node_features" for SLURM node_features) and <method> is a
 * description of how this plugin satisfies that application.  SLURM will only
 * load a node_features plugin if the plugin_type string has a prefix of
 * "node_features/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */
const char plugin_name[]        = "node_features knl_cray plugin";
const char plugin_type[]        = "node_features/knl_cray";
const uint32_t plugin_version   = SLURM_VERSION_NUMBER;

/* Configuration Paramters */
static char *capmc_path = NULL;
static uint32_t capmc_poll_freq = 45;	/* capmc state polling frequency */
static uint32_t capmc_timeout = 0;	/* capmc command timeout in msec */
static char *cnselect_path = NULL;
static bool  debug_flag = false;
static uint16_t allow_mcdram = KNL_MCDRAM_FLAG;
static uint16_t allow_numa = KNL_NUMA_FLAG;
static uid_t *allowed_uid = NULL;
static int allowed_uid_cnt = 0;
static uint16_t default_mcdram = KNL_CACHE;
static uint16_t default_numa = KNL_ALL2ALL;
static char *syscfg_path = NULL;
static pthread_mutex_t config_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool reconfig = false;

/* Percentage of MCDRAM used for cache by type, updated from capmc */
static int mcdram_pct[KNL_MCDRAM_CNT];
static uint64_t *mcdram_per_node = NULL;

/* NOTE: New knl_cray.conf parameters added below must also be added to the
 * contribs/cray/capmc_suspend.c and contribs/cray/capmc_resume.c files */
static s_p_options_t knl_conf_file_options[] = {
	{"AllowMCDRAM", S_P_STRING},
	{"AllowNUMA", S_P_STRING},
	{"AllowUserBoot", S_P_STRING},
	{"CapmcPath", S_P_STRING},
	{"CapmcPollFreq", S_P_UINT32},
	{"CapmcTimeout", S_P_UINT32},
	{"CnselectPath", S_P_STRING},
	{"DefaultMCDRAM", S_P_STRING},
	{"DefaultNUMA", S_P_STRING},
	{"LogFile", S_P_STRING},
	{"SyscfgPath", S_P_STRING},
	{NULL}
};

typedef struct mcdram_cap {
	uint32_t nid;
	char *mcdram_cfg;
} mcdram_cap_t;

typedef struct mcdram_cfg {
	uint64_t dram_size;
	uint32_t nid;
	char *mcdram_cfg;
	uint64_t mcdram_size;
	uint16_t mcdram_pct;
} mcdram_cfg_t;

typedef struct mcdram_cfg2 {
	int hbm_pct;
	char *mcdram_cfg;
	char *nid_str;
	bitstr_t *node_bitmap;
} mcdram_cfg2_t;

typedef struct numa_cap {
	uint32_t nid;
	char *numa_cfg;
} numa_cap_t;

typedef struct numa_cfg {
	uint32_t nid;
	char *numa_cfg;
} numa_cfg_t;

typedef struct numa_cfg2 {
	char *nid_str;
	bitstr_t *node_bitmap;
	char *numa_cfg;
} numa_cfg2_t;

static s_p_hashtbl_t *_config_make_tbl(char *filename);
static void _free_script_argv(char **script_argv);
static mcdram_cap_t *_json_parse_mcdram_cap_array(json_object *jobj, char *key,
						  int *num);
static mcdram_cfg_t *_json_parse_mcdram_cfg_array(json_object *jobj, char *key,
						  int *num);
static void _json_parse_mcdram_cap_object(json_object *jobj, mcdram_cap_t *ent);
static void _json_parse_mcdram_cfg_object(json_object *jobj, mcdram_cfg_t *ent);
static numa_cap_t *_json_parse_numa_cap_array(json_object *jobj, char *key,
					      int *num);
static void _json_parse_numa_cap_object(json_object *jobj, numa_cap_t *ent);
static numa_cfg_t *_json_parse_numa_cfg_array(json_object *jobj, char *key,
					      int *num);
static void _json_parse_numa_cfg_object(json_object *jobj, numa_cfg_t *ent);
static int  _knl_mcdram_bits_cnt(uint16_t mcdram_num);
static uint16_t _knl_mcdram_parse(char *mcdram_str, char *sep);
static char *_knl_mcdram_str(uint16_t mcdram_num);
static uint16_t _knl_mcdram_token(char *token);
static int _knl_numa_bits_cnt(uint16_t numa_num);
static uint16_t _knl_numa_parse(char *numa_str, char *sep);
static char *_knl_numa_str(uint16_t numa_num);
static uint16_t _knl_numa_token(char *token);
static mcdram_cfg2_t *_load_current_mcdram(int *num);
static numa_cfg2_t *_load_current_numa(int *num);
static char *_load_mcdram_type(int hbm_pct);
static char *_load_numa_type(char *type);
static void _log_script_argv(char **script_argv, char *resp_msg);
static void _mcdram_cap_free(mcdram_cap_t *mcdram_cap, int mcdram_cap_cnt);
static void _mcdram_cap_log(mcdram_cap_t *mcdram_cap, int mcdram_cap_cnt);
static void _mcdram_cfg_free(mcdram_cfg_t *mcdram_cfg, int mcdram_cfg_cnt);
static void _mcdram_cfg2_free(mcdram_cfg2_t *mcdram_cfg2, int mcdram_cfg2_cnt);
static void _mcdram_cfg_log(mcdram_cfg_t *mcdram_cfg, int mcdram_cfg_cnt);
static void _merge_strings(char **node_features, char *node_cfg,
			   uint16_t allow_types);
static void _numa_cap_free(numa_cap_t *numa_cap, int numa_cap_cnt);
static void _numa_cap_log(numa_cap_t *numa_cap, int numa_cap_cnt);
static void _numa_cfg_free(numa_cfg_t *numa_cfg, int numa_cfg_cnt);
static void _numa_cfg2_free(numa_cfg2_t *numa_cfg, int numa_cfg2_cnt);
static void _numa_cfg_log(numa_cfg_t *numa_cfg, int numa_cfg_cnt);
static void _numa_cfg2_log(numa_cfg2_t *numa_cfg, int numa_cfg2_cnt);
static uint64_t _parse_size(char *size_str);
static char *_run_script(char *cmd_path, char **script_argv, int *status);
static void _strip_knl_opts(char **features);
static int  _tot_wait (struct timeval *start_time);
static void _update_all_node_features(
				mcdram_cap_t *mcdram_cap, int mcdram_cap_cnt,
				mcdram_cfg_t *mcdram_cfg, int mcdram_cfg_cnt,
				numa_cap_t *numa_cap, int numa_cap_cnt,
				numa_cfg_t *numa_cfg, int numa_cfg_cnt);
static void _update_mcdram_pct(char *tok, int mcdram_num);
static void _update_node_features(struct node_record *node_ptr,
				  mcdram_cap_t *mcdram_cap, int mcdram_cap_cnt,
				  mcdram_cfg_t *mcdram_cfg, int mcdram_cfg_cnt,
				  numa_cap_t *numa_cap, int numa_cap_cnt,
				  numa_cfg_t *numa_cfg, int numa_cfg_cnt);

static s_p_hashtbl_t *_config_make_tbl(char *filename)
{
	s_p_hashtbl_t *tbl = NULL;

	xassert(filename);

	if (!(tbl = s_p_hashtbl_create(knl_conf_file_options))) {
		error("knl.conf: %s: s_p_hashtbl_create error: %m", __func__);
		return tbl;
	}

	if (s_p_parse_file(tbl, NULL, filename, false) == SLURM_ERROR) {
		error("knl.conf: %s: s_p_parse_file error: %m", __func__);
		s_p_hashtbl_destroy(tbl);
		tbl = NULL;
	}

	return tbl;
}

/*
 * Return the count of MCDRAM bits set
 */
static int _knl_mcdram_bits_cnt(uint16_t mcdram_num)
{
	int cnt = 0, i;
	uint16_t tmp = 1;

	for (i = 0; i < 16; i++) {
		if ((mcdram_num & KNL_MCDRAM_FLAG) & tmp)
			cnt++;
		tmp = tmp << 1;
	}
	return cnt;
}

/*
 * Translate KNL MCDRAM string to equivalent numeric value
 * mcdram_str IN - String to scan
 * sep IN - token separator to search for
 * RET MCDRAM numeric value
 */
static uint16_t _knl_mcdram_parse(char *mcdram_str, char *sep)
{
	char *save_ptr = NULL, *tmp, *tok;
	uint16_t mcdram_num = 0;

	if (!mcdram_str)
		return mcdram_num;

	tmp = xstrdup(mcdram_str);
	tok = strtok_r(tmp, sep, &save_ptr);
	while (tok) {
		mcdram_num |= _knl_mcdram_token(tok);
		tok = strtok_r(NULL, sep, &save_ptr);
	}
	xfree(tmp);

	return mcdram_num;
}

/*
 * Translate KNL MCDRAM number to equivalent string value
 * Caller must free return value
 */
static char *_knl_mcdram_str(uint16_t mcdram_num)
{
	char *mcdram_str = NULL, *sep = "";

	if (mcdram_num & KNL_CACHE) {
		xstrfmtcat(mcdram_str, "%scache", sep);
		sep = ",";
	}
	if (mcdram_num & KNL_SPLIT) {
		xstrfmtcat(mcdram_str, "%ssplit", sep);
		sep = ",";
	}
	if (mcdram_num & KNL_FLAT) {
		xstrfmtcat(mcdram_str, "%sflat", sep);
		sep = ",";
	}
	if (mcdram_num & KNL_EQUAL) {
		xstrfmtcat(mcdram_str, "%sequal", sep);
//		sep = ",";	/* Remove to avoid CLANG error */
	}

	return mcdram_str;
}

/*
 * Given a KNL MCDRAM token, return its equivalent numeric value
 * token IN - String to scan
 * RET MCDRAM numeric value
 */
static uint16_t _knl_mcdram_token(char *token)
{
	uint16_t mcdram_num = 0;

	if (!xstrcasecmp(token, "cache"))
		mcdram_num = KNL_CACHE;
	else if (!xstrcasecmp(token, "split"))
		mcdram_num = KNL_SPLIT;
	else if (!xstrcasecmp(token, "flat"))
		mcdram_num = KNL_FLAT;
	else if (!xstrcasecmp(token, "equal"))
		mcdram_num = KNL_EQUAL;

	return mcdram_num;
}

/*
 * Return the count of NUMA bits set
 */
static int _knl_numa_bits_cnt(uint16_t numa_num)
{
	int cnt = 0, i;
	uint16_t tmp = 1;

	for (i = 0; i < 16; i++) {
		if ((numa_num & KNL_NUMA_FLAG) & tmp)
			cnt++;
		tmp = tmp << 1;
	}
	return cnt;
}

/*
 * Translate KNL NUMA string to equivalent numeric value
 * numa_str IN - String to scan
 * sep IN - token separator to search for
 * RET NUMA numeric value
 */
static uint16_t _knl_numa_parse(char *numa_str, char *sep)
{
	char *save_ptr = NULL, *tmp, *tok;
	uint16_t numa_num = 0;

	if (!numa_str)
		return numa_num;

	tmp = xstrdup(numa_str);
	tok = strtok_r(tmp, sep, &save_ptr);
	while (tok) {
		numa_num |= _knl_numa_token(tok);
		tok = strtok_r(NULL, sep, &save_ptr);
	}
	xfree(tmp);

	return numa_num;
}

/*
 * Translate KNL NUMA number to equivalent string value
 * Caller must free return value
 */
static char *_knl_numa_str(uint16_t numa_num)
{
	char *numa_str = NULL, *sep = "";

	if (numa_num & KNL_ALL2ALL) {
		xstrfmtcat(numa_str, "%sa2a", sep);
		sep = ",";
	}
	if (numa_num & KNL_SNC2) {
		xstrfmtcat(numa_str, "%ssnc2", sep);
		sep = ",";
	}
	if (numa_num & KNL_SNC4) {
		xstrfmtcat(numa_str, "%ssnc4", sep);
		sep = ",";
	}
	if (numa_num & KNL_HEMI) {
		xstrfmtcat(numa_str, "%shemi", sep);
		sep = ",";
	}
	if (numa_num & KNL_QUAD) {
		xstrfmtcat(numa_str, "%squad", sep);
//		sep = ",";	/* Remove to avoid CLANG error */
	}

	return numa_str;

}

/*
 * Given a KNL NUMA token, return its equivalent numeric value
 * token IN - String to scan
 * RET NUMA numeric value
 */
static uint16_t _knl_numa_token(char *token)
{
	uint16_t numa_num = 0;

	if (!xstrcasecmp(token, "a2a"))
		numa_num |= KNL_ALL2ALL;
	else if (!xstrcasecmp(token, "snc2"))
		numa_num |= KNL_SNC2;
	else if (!xstrcasecmp(token, "snc4"))
		numa_num |= KNL_SNC4;
	else if (!xstrcasecmp(token, "hemi"))
		numa_num |= KNL_HEMI;
	else if (!xstrcasecmp(token, "quad"))
		numa_num |= KNL_QUAD;

	return numa_num;
}

/* Remove all KNL feature names from the "features" string */
static void _strip_knl_opts(char **features)
{
	char *save_ptr = NULL, *tok;
	char *tmp_str, *result_str = NULL, *sep = "";

	if (*features == NULL)
		return;

	tmp_str = xstrdup(*features);
	tok = strtok_r(tmp_str, ",", &save_ptr);
	while (tok) {
		if (!_knl_mcdram_token(tok) && !_knl_numa_token(tok)) {
			xstrfmtcat(result_str, "%s%s", sep, tok);
			sep = ",";
		}
		tok = strtok_r(NULL, ",", &save_ptr);
	}
	xfree(tmp_str);
	xfree(*features);
	*features = result_str;
}

/*
 * Return time in msec since "start time"
 */
static int _tot_wait (struct timeval *start_time)
{
	struct timeval end_time;
	int msec_delay;

	gettimeofday(&end_time, NULL);
	msec_delay =   (end_time.tv_sec  - start_time->tv_sec ) * 1000;
	msec_delay += ((end_time.tv_usec - start_time->tv_usec + 500) / 1000);
	return msec_delay;
}

/* Free an array of xmalloced records. The array must be NULL terminated. */
static void _free_script_argv(char **script_argv)
{
	int i;

	for (i = 0; script_argv[i]; i++)
		xfree(script_argv[i]);
	xfree(script_argv);
}

/* Update our mcdram_pct array with new data.
 * tok IN - percentage of MCDRAM to be used as cache (string form)
 * mcdram_num - MCDRAM value (bit from KNL_FLAT, etc.)
 */
static void _update_mcdram_pct(char *tok, int mcdram_num)
{
	static int mcdram_set = 0;
	int inx;

	if (mcdram_set == KNL_MCDRAM_CNT)
		return;

	for (inx = 0; inx < KNL_MCDRAM_CNT; inx++) {
		if ((KNL_CACHE << inx) == mcdram_num)
			break;
	}
	if ((inx >= KNL_MCDRAM_CNT) || (mcdram_pct[inx] != -1))
		return;
	mcdram_pct[inx] = strtol(tok, NULL, 10);
	mcdram_set++;
}

static void _json_parse_mcdram_cap_object(json_object *jobj, mcdram_cap_t *ent)
{
	enum json_type type;
	struct json_object_iter iter;
	int64_t x;
	const char *p;
	char *tmp_str, *tok, *save_ptr = NULL, *sep = "";
	int last_mcdram_num = -1;

	json_object_object_foreachC(jobj, iter) {
		type = json_object_get_type(iter.val);
		switch (type) {
		case json_type_int:
			x = json_object_get_int64(iter.val);
			if (xstrcmp(iter.key, "nid") == 0) {
				ent->nid = x;
			}
			break;
		case json_type_string:
			p = json_object_get_string(iter.val);
			if (xstrcmp(iter.key, "mcdram_cfg") == 0) {
				tmp_str = xstrdup(p);
				tok = strtok_r(tmp_str, ",", &save_ptr);
				while (tok) {
					if ((tok[0] >= '0') && (tok[0] <= '9')){
						_update_mcdram_pct(tok,
							last_mcdram_num);
						last_mcdram_num = -1;
					} else {
						last_mcdram_num =
							_knl_mcdram_token(tok);
						xstrfmtcat(ent->mcdram_cfg,
							   "%s%s", sep, tok);
						sep = ",";
					}
					tok = strtok_r(NULL, ",", &save_ptr);
				}
				xfree(tmp_str);
			}
			break;
		default:
			break;
		}
	}
}

static uint64_t _parse_size(char *size_str)
{
	uint64_t size_num = 0;
	char *end_ptr = NULL;

	size_num = (uint64_t) strtol(size_str, &end_ptr, 10);
	if ((end_ptr[0] == 'k') || (end_ptr[0] == 'K'))
		size_num *= 1024;
	else if ((end_ptr[0] == 'm') || (end_ptr[0] == 'M'))
		size_num *= (1024 * 1024);
	else if ((end_ptr[0] == 'g') || (end_ptr[0] == 'G'))
		size_num *= (1024 * 1024 * 1024);
	else if (end_ptr[0] != '\0')
		info("Invalid MCDRAM size: %s", size_str);

	return size_num;
}

static void _json_parse_mcdram_cfg_object(json_object *jobj, mcdram_cfg_t *ent)
{
	enum json_type type;
	struct json_object_iter iter;
	int64_t x;
	const char *p;

	json_object_object_foreachC(jobj, iter) {
		type = json_object_get_type(iter.val);
		switch (type) {
		case json_type_int:
			x = json_object_get_int64(iter.val);
			if (xstrcmp(iter.key, "nid") == 0) {
				ent->nid = x;
			}
			break;
		case json_type_string:
			p = json_object_get_string(iter.val);
			if (xstrcmp(iter.key, "dram_size") == 0) {
				ent->dram_size = _parse_size((char *) p);
			} else if (xstrcmp(iter.key, "mcdram_cfg") == 0) {
				ent->mcdram_cfg = xstrdup(p);
			} else if (xstrcmp(iter.key, "mcdram_pct") == 0) {
				ent->mcdram_pct = _parse_size((char *) p);
			} else if (xstrcmp(iter.key, "mcdram_size") == 0) {
				ent->mcdram_size = _parse_size((char *) p);
			}
			break;
		default:
			break;
		}
	}
}

static void _json_parse_numa_cap_object(json_object *jobj, numa_cap_t *ent)
{
	enum json_type type;
	struct json_object_iter iter;
	int64_t x;
	const char *p;

	json_object_object_foreachC(jobj, iter) {
		type = json_object_get_type(iter.val);
		switch (type) {
		case json_type_int:
			x = json_object_get_int64(iter.val);
			if (xstrcmp(iter.key, "nid") == 0) {
				ent->nid = x;
			}
			break;
		case json_type_string:
			p = json_object_get_string(iter.val);
			if (xstrcmp(iter.key, "numa_cfg") == 0) {
				ent->numa_cfg = xstrdup(p);
			}
			break;
		default:
			break;
		}
	}
}

static void _json_parse_numa_cfg_object(json_object *jobj, numa_cfg_t *ent)
{
	enum json_type type;
	struct json_object_iter iter;
	int64_t x;
	const char *p;

	json_object_object_foreachC(jobj, iter) {
		type = json_object_get_type(iter.val);
		switch (type) {
		case json_type_int:
			x = json_object_get_int64(iter.val);
			if (xstrcmp(iter.key, "nid") == 0) {
				ent->nid = x;
			}
			break;
		case json_type_string:
			p = json_object_get_string(iter.val);
			if (xstrcmp(iter.key, "numa_cfg") == 0) {
				ent->numa_cfg = xstrdup(p);
			}
			break;
		default:
			break;
		}
	}
}

static mcdram_cap_t *_json_parse_mcdram_cap_array(json_object *jobj, char *key,
						  int *num)
{
	json_object *jarray;
	json_object *jvalue;
	mcdram_cap_t *ents;
	int i;

	jarray = jobj;
	json_object_object_get_ex(jobj, key, &jarray);

	*num = json_object_array_length(jarray);
	ents = xmalloc(*num * sizeof(mcdram_cap_t));

	for (i = 0; i < *num; i++) {
		jvalue = json_object_array_get_idx(jarray, i);
		_json_parse_mcdram_cap_object(jvalue, &ents[i]);
	}

	return ents;
}

/* Return NID string for all nodes with specified MCDRAM mode (HBM percentage).
 * NOTE: Information not returned for nodes which are not up
 * NOTE: xfree() the return value. */
static char *_load_mcdram_type(int hbm_pct)
{
	char **script_argv, *resp_msg;
	int i, status = 0;
	DEF_TIMERS;

	if (hbm_pct < 0)	/* Unsupported configuration on this system */
		return NULL;
	script_argv = xmalloc(sizeof(char *) * 4);	/* NULL terminated */
	script_argv[0] = xstrdup("cnselect");
	script_argv[1] = xstrdup("-e");
	xstrfmtcat(script_argv[2], "hbmcachepct.eq.%d", hbm_pct);
	START_TIMER;
	resp_msg = _run_script(cnselect_path, script_argv, &status);
	END_TIMER;
	if (debug_flag) {
		info("%s: %s %s %s ran for %s", __func__,
		     script_argv[0], script_argv[1], script_argv[2], TIME_STR);
	}
	if (resp_msg == NULL) {
		debug("%s: %s %s %s returned no information",
		      __func__, script_argv[0], script_argv[1], script_argv[2]);
	} else {
		i = strlen(resp_msg);
		if (resp_msg[i-1] == '\n')
			resp_msg[i-1] = '\0';
	}
	_log_script_argv(script_argv, resp_msg);
	_free_script_argv(script_argv);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		error("%s: %s %s %s status:%u response:%s", __func__,
		      script_argv[0], script_argv[1], script_argv[2],
		      status, resp_msg);
	}
	return resp_msg;
}

/* Return table of MCDRAM modes and NID string identifying nodes with that mode.
 * Use _mcdram_cfg2_free() to release returned data structure */
static mcdram_cfg2_t *_load_current_mcdram(int *num)
{
	mcdram_cfg2_t *mcdram_cfg;
	int i;

	mcdram_cfg = xmalloc(sizeof(mcdram_cfg2_t) * 4);

	for (i = 0; i < 4; i++) {
		mcdram_cfg[i].hbm_pct = mcdram_pct[i];
		mcdram_cfg[i].mcdram_cfg = _knl_mcdram_str(KNL_CACHE << i);
		mcdram_cfg[i].nid_str = _load_mcdram_type(mcdram_cfg[i].hbm_pct);
		if (mcdram_cfg[i].nid_str && mcdram_cfg[i].nid_str[0]) {
			mcdram_cfg[i].node_bitmap = bit_alloc(100000);
			(void) bit_unfmt(mcdram_cfg[i].node_bitmap,
					 mcdram_cfg[i].nid_str);
		}
	}
	*num = 4;
	return mcdram_cfg;
}

/* Return NID string for all nodes with specified NUMA mode.
 * NOTE: Information not returned for nodes which are not up
 * NOTE: xfree() the return value. */
static char *_load_numa_type(char *type)
{
	char **script_argv, *resp_msg;
	int i, status = 0;
	DEF_TIMERS;

	script_argv = xmalloc(sizeof(char *) * 4);	/* NULL terminated */
	script_argv[0] = xstrdup("cnselect");
	script_argv[1] = xstrdup("-e");
	xstrfmtcat(script_argv[2], "numa_cfg.eq.%s", type);
	START_TIMER;
	resp_msg = _run_script(cnselect_path, script_argv, &status);
	END_TIMER;
	if (debug_flag) {
		info("%s: %s %s %s ran for %s", __func__,
		     script_argv[0], script_argv[1], script_argv[2], TIME_STR);
	}
	if (resp_msg == NULL) {
		debug("%s: %s %s %s returned no information",
		      __func__, script_argv[0], script_argv[1], script_argv[2]);
	} else {
		i = strlen(resp_msg);
		if (resp_msg[i-1] == '\n')
			resp_msg[i-1] = '\0';
	}
	_log_script_argv(script_argv, resp_msg);
	_free_script_argv(script_argv);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		error("%s: %s %s %s status:%u response:%s", __func__,
		      script_argv[0], script_argv[1], script_argv[2],
		      status, resp_msg);
	}
	return resp_msg;
}

/* Return table of NUMA modes and NID string identifying nodes with that mode.
 * Use _numa_cfg2_free() to release returned data structure */
static numa_cfg2_t *_load_current_numa(int *num)
{
	numa_cfg2_t *numa_cfg2;
	int i;

	numa_cfg2 = xmalloc(sizeof(numa_cfg2_t) * 5);
	numa_cfg2[0].numa_cfg = xstrdup("a2a");
	numa_cfg2[1].numa_cfg = xstrdup("snc2");
	numa_cfg2[2].numa_cfg = xstrdup("snc4");
	numa_cfg2[3].numa_cfg = xstrdup("hemi");
	numa_cfg2[4].numa_cfg = xstrdup("quad");

	for (i = 0; i < 5; i++) {
		numa_cfg2[i].nid_str = _load_numa_type(numa_cfg2[i].numa_cfg);
		if (numa_cfg2[i].nid_str && numa_cfg2[i].nid_str[0]) {
			numa_cfg2[i].node_bitmap = bit_alloc(100000);
			(void) bit_unfmt(numa_cfg2[i].node_bitmap,
					 numa_cfg2[i].nid_str);
		}
	}
	*num = 5;
	return numa_cfg2;
}

static mcdram_cfg_t *_json_parse_mcdram_cfg_array(json_object *jobj, char *key,
						  int *num)
{
	json_object *jarray;
	json_object *jvalue;
	mcdram_cfg_t *ents;
	int i;

	jarray = jobj;
	json_object_object_get_ex(jobj, key, &jarray);

	*num = json_object_array_length(jarray);
	ents = xmalloc(*num * sizeof(mcdram_cfg_t));

	for (i = 0; i < *num; i++) {
		jvalue = json_object_array_get_idx(jarray, i);
		_json_parse_mcdram_cfg_object(jvalue, &ents[i]);
	}

	return ents;
}

static numa_cap_t *_json_parse_numa_cap_array(json_object *jobj, char *key,
					      int *num)
{
	json_object *jarray;
	json_object *jvalue;
	numa_cap_t *ents;
	int i;

	jarray = jobj;
	json_object_object_get_ex(jobj, key, &jarray);

	*num = json_object_array_length(jarray);
	ents = xmalloc(*num * sizeof(numa_cap_t));

	for (i = 0; i < *num; i++) {
		jvalue = json_object_array_get_idx(jarray, i);
		_json_parse_numa_cap_object(jvalue, &ents[i]);
	}

	return ents;
}

static numa_cfg_t *_json_parse_numa_cfg_array(json_object *jobj, char *key,
					      int *num)
{
	json_object *jarray;
	json_object *jvalue;
	numa_cfg_t *ents;
	int i;

	jarray = jobj;
	json_object_object_get_ex(jobj, key, &jarray);

	*num = json_object_array_length(jarray);
	ents = xmalloc(*num * sizeof(numa_cfg_t));

	for (i = 0; i < *num; i++) {
		jvalue = json_object_array_get_idx(jarray, i);
		_json_parse_numa_cfg_object(jvalue, &ents[i]);
	}

	return ents;
}

/* Log a command's arguments. */
static void _log_script_argv(char **script_argv, char *resp_msg)
{
	char *cmd_line = NULL;
	int i;

	if (!debug_flag)
		return;

	for (i = 0; script_argv[i]; i++) {
		if (i)
			xstrcat(cmd_line, " ");
		xstrcat(cmd_line, script_argv[i]);
	}
	info("%s", cmd_line);
	if (resp_msg && resp_msg[0])
		info("%s", resp_msg);
	xfree(cmd_line);
}

static void _mcdram_cap_free(mcdram_cap_t *mcdram_cap, int mcdram_cap_cnt)
{
	int i;

	if (!mcdram_cap)
		return;
	for (i = 0; i < mcdram_cap_cnt; i++) {
		xfree(mcdram_cap[i].mcdram_cfg);
	}
	xfree(mcdram_cap);
}

static void _mcdram_cap_log(mcdram_cap_t *mcdram_cap, int mcdram_cap_cnt)
{
	int i;

	if (!mcdram_cap)
		return;
	for (i = 0; i < mcdram_cap_cnt; i++) {
		info("MCDRAM_CAP[%d]: nid:%u mcdram_cfg:%s",
		     i, mcdram_cap[i].nid, mcdram_cap[i].mcdram_cfg);
	}
}

static void _mcdram_cfg_free(mcdram_cfg_t *mcdram_cfg, int mcdram_cfg_cnt)
{
	int i;

	if (!mcdram_cfg)
		return;
	for (i = 0; i < mcdram_cfg_cnt; i++) {
		xfree(mcdram_cfg[i].mcdram_cfg);
	}
	xfree(mcdram_cfg);
}

static void _mcdram_cfg2_free(mcdram_cfg2_t *mcdram_cfg2, int mcdram_cfg2_cnt)
{
	int i;

	if (!mcdram_cfg2)
		return;
	for (i = 0; i < mcdram_cfg2_cnt; i++) {
		xfree(mcdram_cfg2[i].mcdram_cfg);
		FREE_NULL_BITMAP(mcdram_cfg2[i].node_bitmap);
		xfree(mcdram_cfg2[i].nid_str);
	}
	xfree(mcdram_cfg2);
}

static void _mcdram_cfg_log(mcdram_cfg_t *mcdram_cfg, int mcdram_cfg_cnt)
{
	int i;

	if (!mcdram_cfg)
		return;
	for (i = 0; i < mcdram_cfg_cnt; i++) {
		info("MCDRAM_CFG[%d]: nid:%u dram_size:%"PRIu64" mcdram_cfg:%s mcdram_pct:%u mcdram_size:%"PRIu64,
		     i, mcdram_cfg[i].nid, mcdram_cfg[i].dram_size,
		     mcdram_cfg[i].mcdram_cfg, mcdram_cfg[i].mcdram_pct,
		     mcdram_cfg[i].mcdram_size);
	}
}

static void _mcdram_cfg2_log(mcdram_cfg2_t *mcdram_cfg2, int mcdram_cfg2_cnt)
{
	int i;

	if (!mcdram_cfg2)
		return;
	for (i = 0; i < mcdram_cfg2_cnt; i++) {
		info("MCDRAM_CFG[%d]: nid_str:%s mcdram_cfg:%s hbm_pct:%d",
		     i, mcdram_cfg2[i].nid_str, mcdram_cfg2[i].mcdram_cfg,
		     mcdram_cfg2[i].hbm_pct);
	}
}

static void _numa_cap_free(numa_cap_t *numa_cap, int numa_cap_cnt)
{
	int i;

	if (!numa_cap)
		return;
	for (i = 0; i < numa_cap_cnt; i++) {
		xfree(numa_cap[i].numa_cfg);
	}
	xfree(numa_cap);
}

static void _numa_cap_log(numa_cap_t *numa_cap, int numa_cap_cnt)
{
	int i;

	if (!numa_cap)
		return;
	for (i = 0; i < numa_cap_cnt; i++) {
		info("NUMA_CAP[%d]: nid:%u numa_cfg:%s",
		     i, numa_cap[i].nid, numa_cap[i].numa_cfg);
	}
}

static void _numa_cfg_free(numa_cfg_t *numa_cfg, int numa_cfg_cnt)
{
	int i;

	if (!numa_cfg)
		return;
	for (i = 0; i < numa_cfg_cnt; i++) {
		xfree(numa_cfg[i].numa_cfg);
	}
	xfree(numa_cfg);
}

static void _numa_cfg2_free(numa_cfg2_t *numa_cfg2, int numa_cfg2_cnt)
{
	int i;

	if (!numa_cfg2)
		return;
	for (i = 0; i < numa_cfg2_cnt; i++) {
		xfree(numa_cfg2[i].nid_str);
		xfree(numa_cfg2[i].numa_cfg);
		FREE_NULL_BITMAP(numa_cfg2[i].node_bitmap);
	}
	xfree(numa_cfg2);
}

static void _numa_cfg_log(numa_cfg_t *numa_cfg, int numa_cfg_cnt)
{
	int i;

	if (!numa_cfg)
		return;
	for (i = 0; i < numa_cfg_cnt; i++) {
		info("NUMA_CFG[%d]: nid:%u numa_cfg:%s",
		     i, numa_cfg[i].nid, numa_cfg[i].numa_cfg);
	}
}

static void _numa_cfg2_log(numa_cfg2_t *numa_cfg2, int numa_cfg2_cnt)
{
	int i;

	if (!numa_cfg2)
		return;
	for (i = 0; i < numa_cfg2_cnt; i++) {
		info("NUMA_CFG[%d]: nid_str:%s numa_cfg:%s",
		     i, numa_cfg2[i].nid_str, numa_cfg2[i].numa_cfg);
	}
}

/* Run a script and return its stdout plus exit status */
static char *_run_script(char *cmd_path, char **script_argv, int *status)
{
	int cc, i, new_wait, resp_size = 0, resp_offset = 0;
	pid_t cpid;
	char *resp = NULL;
	int pfd[2] = { -1, -1 };

	if (access(cmd_path, R_OK | X_OK) < 0) {
		error("%s: %s can not be executed: %m", __func__, cmd_path);
		*status = 127;
		resp = xstrdup("Slurm node_features/knl_cray configuration error");
		return resp;
	}
	if (pipe(pfd) != 0) {
		error("%s: pipe(): %m", __func__);
		*status = 127;
		resp = xstrdup("System error");
		return resp;
	}

	if ((cpid = fork()) == 0) {
		cc = sysconf(_SC_OPEN_MAX);
		dup2(pfd[1], STDERR_FILENO);
		dup2(pfd[1], STDOUT_FILENO);
		for (i = 0; i < cc; i++) {
			if ((i != STDERR_FILENO) && (i != STDOUT_FILENO))
				close(i);
		}
		setpgid(0, 0);
		execv(cmd_path, script_argv);
		error("%s: execv(%s): %m", __func__, cmd_path);
		exit(127);
	} else if (cpid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		error("%s: fork(): %m", __func__);
	} else {
		struct pollfd fds;
		struct timeval tstart;
		resp_size = 1024;
		resp = xmalloc(resp_size);
		close(pfd[1]);
		gettimeofday(&tstart, NULL);
		while (1) {
			if (slurmctld_config.shutdown_time) {
				error("%s: killing %s operation on shutdown",
				      __func__, script_argv[1]);
				break;
			}
			fds.fd = pfd[0];
			fds.events = POLLIN | POLLHUP | POLLRDHUP;
			fds.revents = 0;
			new_wait = capmc_timeout - _tot_wait(&tstart);
			if (new_wait <= 0) {
				error("%s: %s poll timeout @ %d msec",
				      __func__, script_argv[1], capmc_timeout);
				break;
			}
			new_wait = MIN(new_wait, MAX_POLL_WAIT);
			i = poll(&fds, 1, new_wait);
			if (i == 0) {
				continue;
			} else if (i < 0) {
				error("%s: %s poll:%m", __func__,
				      script_argv[1]);
				break;
			}
			if ((fds.revents & POLLIN) == 0)
				break;
			i = read(pfd[0], resp + resp_offset,
				 resp_size - resp_offset);
			if (i == 0) {
				break;
			} else if (i < 0) {
				if (errno == EAGAIN)
					continue;
				error("%s: read(%s): %m", __func__, capmc_path);
				break;
			} else {
				resp_offset += i;
				if (resp_offset + 1024 >= resp_size) {
					resp_size *= 2;
					resp = xrealloc(resp, resp_size);
				}
			}
		}
		killpg(cpid, SIGTERM);
		usleep(10000);
		killpg(cpid, SIGKILL);
		waitpid(cpid, status, 0);
		close(pfd[0]);
	}
	return resp;
}

static void _merge_strings(char **node_features, char *node_cfg,
			   uint16_t allow_types)
{
	char *tmp_str1, *tok1, *save_ptr1 = NULL;
	char *tmp_str2, *tok2, *save_ptr2 = NULL;
	bool mcdram_filter = false, numa_filter = false;

	if ((node_cfg == NULL) || (node_cfg[0] == '\0'))
		return;
	if (*node_features == NULL) {
		*node_features = xstrdup(node_cfg);
		return;
	}

	if ((allow_types &  KNL_MCDRAM_FLAG) &&
	    (allow_types != KNL_MCDRAM_FLAG))
		mcdram_filter = true;
	if ((allow_types &  KNL_NUMA_FLAG) &&
	    (allow_types != KNL_NUMA_FLAG))
		numa_filter = true;

	/* Merge strings and avoid duplicates */
	tmp_str1 = xstrdup(node_cfg);
	tok1 = strtok_r(tmp_str1, ",", &save_ptr1);
	while (tok1) {
		bool match = false;
		if (mcdram_filter &&
		    ((_knl_mcdram_token(tok1) & allow_types) == 0))
			goto next_tok;
		if (numa_filter &&
		    ((_knl_numa_token(tok1) & allow_types) == 0))
			goto next_tok;
		tmp_str2 = xstrdup(*node_features);
		tok2 = strtok_r(tmp_str2, ",", &save_ptr2);
		while (tok2) {
			if (!xstrcmp(tok1, tok2)) {
				match = true;
				break;
			}
			tok2 = strtok_r(NULL, ",", &save_ptr2);
		}
		xfree(tmp_str2);
		if (!match)
			xstrfmtcat(*node_features, ",%s", tok1);
next_tok:	tok1 = strtok_r(NULL, ",", &save_ptr1);
	}
	xfree(tmp_str1);
}

/* Update features and features_act fields for ALL nodes based upon
 * its current configuration provided by capmc */
static void _update_all_node_features(
				mcdram_cap_t *mcdram_cap, int mcdram_cap_cnt,
				mcdram_cfg_t *mcdram_cfg, int mcdram_cfg_cnt,
				numa_cap_t *numa_cap, int numa_cap_cnt,
				numa_cfg_t *numa_cfg, int numa_cfg_cnt)
{
	struct node_record *node_ptr;
	char node_name[32], *prefix;
	int i, width = 5;
	uint64_t mcdram_size;

	if ((node_record_table_ptr == NULL) ||
	    (node_record_table_ptr->name == NULL)) {
		prefix = xstrdup("nid");
	} else {
		prefix = xstrdup(node_record_table_ptr->name);
		for (i = 0; prefix[i]; i++) {
			if ((prefix[i] >= '0') && (prefix[i] <= '9')) {
				prefix[i] = '\0';
				width = 1;
				for (i++ ; prefix[i]; i++)
					width++;
				break;
			}
		}
	}
	if (mcdram_cap) {
		for (i = 0; i < mcdram_cap_cnt; i++) {
			snprintf(node_name, sizeof(node_name),
				 "%s%.*d", prefix, width, mcdram_cap[i].nid);
			node_ptr = find_node_record(node_name);
			if (node_ptr) {
				_merge_strings(&node_ptr->features,
					       mcdram_cap[i].mcdram_cfg,
					       allow_mcdram);
			}
		}
	}
	if (mcdram_cfg) {
		for (i = 0; i < mcdram_cfg_cnt; i++) {
			snprintf(node_name, sizeof(node_name),
				 "%s%.*d", prefix, width, mcdram_cfg[i].nid);
			if (!(node_ptr = find_node_record(node_name)))
				continue;
			mcdram_per_node[node_ptr - node_record_table_ptr] =
				mcdram_cfg[i].mcdram_size;
			_merge_strings(&node_ptr->features_act,
				       mcdram_cfg[i].mcdram_cfg,
				       allow_mcdram);
			mcdram_size = mcdram_cfg[i].mcdram_size *
				      (100 - mcdram_cfg[i].mcdram_pct) / 100;
			if (!node_ptr->gres) {
				node_ptr->gres =
					xstrdup(node_ptr->config_ptr->gres);
			}
			gres_plugin_node_feature(node_ptr->name, "hbm",
						 mcdram_size, &node_ptr->gres,
						 &node_ptr->gres_list);
		}
	}
	if (numa_cap) {
		for (i = 0; i < numa_cap_cnt; i++) {
			snprintf(node_name, sizeof(node_name),
				 "%s%.*d", prefix, width, numa_cap[i].nid);
			node_ptr = find_node_record(node_name);
			if (node_ptr) {
				_merge_strings(&node_ptr->features,
					       numa_cap[i].numa_cfg,
					       allow_numa);
			}
		}
	}
	if (numa_cfg) {
		for (i = 0; i < numa_cfg_cnt; i++) {
			snprintf(node_name, sizeof(node_name),
				 "%s%.*u", prefix, width, numa_cfg[i].nid);
			node_ptr = find_node_record(node_name);
			if (node_ptr) {
				_merge_strings(&node_ptr->features_act,
					       numa_cfg[i].numa_cfg,
					       allow_numa);
			}
		}
	}
	xfree(prefix);
}

/* Update a specific node's features and features_act fields based upon
 * its current configuration provided by capmc */
static void _update_node_features(struct node_record *node_ptr,
				  mcdram_cap_t *mcdram_cap, int mcdram_cap_cnt,
				  mcdram_cfg_t *mcdram_cfg, int mcdram_cfg_cnt,
				  numa_cap_t *numa_cap, int numa_cap_cnt,
				  numa_cfg_t *numa_cfg, int numa_cfg_cnt)
{
	int i, nid;
	char *end_ptr = "";
	uint64_t mcdram_size;

	xassert(node_ptr);
	nid = strtol(node_ptr->name + 3, &end_ptr, 10);
	if (end_ptr[0] != '\0') {
		error("%s: Invalid node name (%s)", __func__, node_ptr->name);
		return;
	}

	_strip_knl_opts(&node_ptr->features);
	if (node_ptr->features && !node_ptr->features_act)
		node_ptr->features_act = xstrdup(node_ptr->features);
	_strip_knl_opts(&node_ptr->features_act);

	if (mcdram_cap) {
		for (i = 0; i < mcdram_cap_cnt; i++) {
			if (nid == mcdram_cap[i].nid) {
				_merge_strings(&node_ptr->features,
					       mcdram_cap[i].mcdram_cfg,
					       allow_mcdram);
				break;
			}
		}
	}
	if (mcdram_cfg) {
		for (i = 0; i < mcdram_cfg_cnt; i++) {
			if (nid != mcdram_cfg[i].nid)
				continue;
			_merge_strings(&node_ptr->features_act,
				       mcdram_cfg[i].mcdram_cfg,
				       allow_mcdram);
			mcdram_per_node[node_ptr - node_record_table_ptr] =
				mcdram_cfg[i].mcdram_size;
			mcdram_size = mcdram_cfg[i].mcdram_size *
				      (100 - mcdram_cfg[i].mcdram_pct) / 100;
			if (!node_ptr->gres) {
				node_ptr->gres =
					xstrdup(node_ptr->config_ptr->gres);
			}
			gres_plugin_node_feature(node_ptr->name, "hbm",
						 mcdram_size, &node_ptr->gres,
						 &node_ptr->gres_list);
			break;
		}
	}
	if (numa_cap) {
		for (i = 0; i < numa_cap_cnt; i++) {
			if (nid == numa_cap[i].nid) {
				_merge_strings(&node_ptr->features,
					       numa_cap[i].numa_cfg,
					       allow_numa);
				break;
			}
		}
	}
	if (numa_cfg) {
		for (i = 0; i < numa_cfg_cnt; i++) {
			if (nid == numa_cfg[i].nid) {
				_merge_strings(&node_ptr->features_act,
					       numa_cfg[i].numa_cfg,
					       allow_numa);
				break;
			}
		}
	}
}

static void _make_uid_array(char *uid_str)
{
	char *save_ptr = NULL, *tmp_str, *tok;
	int i, uid_cnt = 0;

	if (!uid_str)
		return;

	/* Count the number of users */
	for (i = 0; uid_str[i]; i++) {
		if (uid_str[i] == ',')
			uid_cnt++;
	}
	uid_cnt++;

	allowed_uid = xmalloc(sizeof(uid_t) * uid_cnt);
	allowed_uid_cnt = 0;
	tmp_str = xstrdup(uid_str);
	tok = strtok_r(tmp_str, ",", &save_ptr);
	while (tok) {
		if (uid_from_string(tok, &allowed_uid[allowed_uid_cnt++]) < 0)
			fatal("knl_cray.conf: Invalid AllowUserBoot: %s", tok);
		tok = strtok_r(NULL, ",", &save_ptr);
	}
	xfree(tmp_str);
}

static char *_make_uid_str(uid_t *uid_array, int uid_cnt)
{
	char *sep = "", *tmp_str = NULL, *uid_str = NULL;
	int i;

	if (allowed_uid_cnt == 0) {
		uid_str = xstrdup("ALL");
		return uid_str;
	}

	for (i = 0; i < uid_cnt; i++) {
		tmp_str = uid_to_string(uid_array[i]);
		xstrfmtcat(uid_str, "%s%s(%d)", sep, tmp_str, uid_array[i]);
		xfree(tmp_str);
		sep = ",";
	}

	return uid_str;
}

/* Load configuration */
extern int init(void)
{
	char *allow_mcdram_str, *allow_numa_str, *allow_user_str;
	char *default_mcdram_str, *default_numa_str;
	char *knl_conf_file, *tmp_str = NULL;
	s_p_hashtbl_t *tbl;
	struct stat stat_buf;
	int i;

	/* Set default values */
	allow_mcdram = KNL_MCDRAM_FLAG;
	allow_numa = KNL_NUMA_FLAG;
	xfree(allowed_uid);
	allowed_uid_cnt = 0;
	xfree(capmc_path);
	capmc_poll_freq = 45;
	capmc_timeout = 1000;
	debug_flag = false;
	default_mcdram = KNL_CACHE;
	default_numa = KNL_ALL2ALL;
	for (i = 0; i < KNL_MCDRAM_CNT; i++)
		mcdram_pct[i] = -1;

	knl_conf_file = get_extra_conf_path("knl_cray.conf");
	if ((stat(knl_conf_file, &stat_buf) == 0) &&
	    (tbl = _config_make_tbl(knl_conf_file))) {
		if (s_p_get_string(&tmp_str, "AllowMCDRAM", tbl)) {
			allow_mcdram = _knl_mcdram_parse(tmp_str, ",");
			if (_knl_mcdram_bits_cnt(allow_mcdram) < 1) {
				fatal("knl_cray.conf: Invalid AllowMCDRAM=%s",
				      tmp_str);
			}
			xfree(tmp_str);
		}
		if (s_p_get_string(&tmp_str, "AllowNUMA", tbl)) {
			allow_numa = _knl_numa_parse(tmp_str, ",");
			if (_knl_numa_bits_cnt(allow_numa) < 1) {
				fatal("knl_cray.conf: Invalid AllowNUMA=%s",
				      tmp_str);
			}
			xfree(tmp_str);
		}
		if (s_p_get_string(&tmp_str, "AllowUserBoot", tbl)) {
			_make_uid_array(tmp_str);
			xfree(tmp_str);
		}
		(void) s_p_get_string(&capmc_path, "CapmcPath", tbl);
		(void) s_p_get_uint32(&capmc_poll_freq, "CapmcPollFreq", tbl);
		(void) s_p_get_uint32(&capmc_timeout, "CapmcTimeout", tbl);
		(void) s_p_get_string(&cnselect_path, "CnselectPath", tbl);
		if (s_p_get_string(&tmp_str, "DefaultMCDRAM", tbl)) {
			default_mcdram = _knl_mcdram_parse(tmp_str, ",");
			if (_knl_mcdram_bits_cnt(default_mcdram) != 1) {
				fatal("knl_cray.conf: Invalid DefaultMCDRAM=%s",
				      tmp_str);
			}
			xfree(tmp_str);
		}
		if (s_p_get_string(&tmp_str, "DefaultNUMA", tbl)) {
			default_numa = _knl_numa_parse(tmp_str, ",");
			if (_knl_numa_bits_cnt(default_numa) != 1) {
				fatal("knl_cray.conf: Invalid DefaultNUMA=%s",
				      tmp_str);
			}
			xfree(tmp_str);
		}
		(void) s_p_get_string(&syscfg_path, "SyscfgPath", tbl);
		s_p_hashtbl_destroy(tbl);
	} else {
		error("something wrong with opening/reading knl_cray.conf");
	}
	xfree(knl_conf_file);
	if (!capmc_path)
		capmc_path = xstrdup("/opt/cray/capmc/default/bin/capmc");
	capmc_timeout = MAX(capmc_timeout, 500);
	if (!cnselect_path)
		cnselect_path = xstrdup("/opt/cray/sdb/default/bin/cnselect");
	if (!syscfg_path)
		verbose("SyscfgPath is not configured");

	if (slurm_get_debug_flags() & DEBUG_FLAG_NODE_FEATURES)
		debug_flag = true;

	if (slurm_get_debug_flags() & DEBUG_FLAG_NODE_FEATURES) {
		allow_mcdram_str = _knl_mcdram_str(allow_mcdram);
		allow_numa_str = _knl_numa_str(allow_numa);
		allow_user_str = _make_uid_str(allowed_uid, allowed_uid_cnt);
		default_mcdram_str = _knl_mcdram_str(default_mcdram);
		default_numa_str = _knl_numa_str(default_numa);
		info("AllowMCDRAM=%s AllowNUMA=%s",
		     allow_mcdram_str, allow_numa_str);
		info("AllowUserBoot=%s", allow_user_str);
		info("CapmcPath=%s", capmc_path);
		info("CapmcPollFreq=%u sec", capmc_poll_freq);
		info("CapmcTimeout=%u msec", capmc_timeout);
		info("CnselectPath=%s", cnselect_path);
		info("DefaultMCDRAM=%s DefaultNUMA=%s",
		     default_mcdram_str, default_numa_str);
		info("SyscfgPath=%s", syscfg_path);
		xfree(allow_mcdram_str);
		xfree(allow_numa_str);
		xfree(allow_user_str);
		xfree(default_mcdram_str);
		xfree(default_numa_str);
	}
	gres_plugin_add("hbm");

	return SLURM_SUCCESS;
}

/* Release allocated memory */
extern int fini(void)
{
	xfree(allowed_uid);
	allowed_uid_cnt = 0;
	xfree(capmc_path);
	xfree(cnselect_path);
	capmc_timeout = 0;
	debug_flag = false;
	xfree(mcdram_per_node);
	xfree(syscfg_path);
	return SLURM_SUCCESS;
}

/* Reload configuration */
extern int node_features_p_reconfig(void)
{
	slurm_mutex_lock(&config_mutex);
	reconfig = true;
	slurm_mutex_unlock(&config_mutex);
	return SLURM_SUCCESS;
}

/* Update active and available features on specified nodes,
 * sets features on all nodes if node_list is NULL */
extern int node_features_p_get_node(char *node_list)
{
	json_object *j;
	json_object_iter iter;
	int i, k, status = 0, rc = SLURM_SUCCESS;
	DEF_TIMERS;
	char *resp_msg, **script_argv;
	mcdram_cap_t *mcdram_cap = NULL;
	mcdram_cfg_t *mcdram_cfg = NULL;
	mcdram_cfg2_t *mcdram_cfg2 = NULL;
	numa_cap_t *numa_cap = NULL;
	numa_cfg_t *numa_cfg = NULL;
	numa_cfg2_t *numa_cfg2 = NULL;
	int mcdram_cap_cnt = 0, mcdram_cfg_cnt = 0, mcdram_cfg2_cnt = 0;
	int numa_cap_cnt = 0, numa_cfg_cnt = 0, numa_cfg2_cnt = 0;
	struct node_record *node_ptr;
	hostlist_t host_list;
	char *node_name;

	slurm_mutex_lock(&config_mutex);
	if (reconfig) {
		(void) init();
		reconfig = true;
	}
	slurm_mutex_unlock(&config_mutex);
	if (!mcdram_per_node)
		mcdram_per_node = xmalloc(sizeof(uint64_t) * node_record_count);

	/*
	 * Load available MCDRAM capabilities
	 */
	script_argv = xmalloc(sizeof(char *) * 4);	/* NULL terminated */
	script_argv[0] = xstrdup("capmc");
	script_argv[1] = xstrdup("get_mcdram_capabilities");
	START_TIMER;
	resp_msg = _run_script(capmc_path, script_argv, &status);
	END_TIMER;
	if (debug_flag) {
		info("%s: get_mcdram_capabilities ran for %s",
		     __func__, TIME_STR);
	}
	_log_script_argv(script_argv, resp_msg);
	_free_script_argv(script_argv);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		error("%s: get_mcdram_capabilities status:%u response:%s",
		      __func__, status, resp_msg);
	}
	if (resp_msg == NULL) {
		info("%s: get_mcdram_capabilities returned no information",
		     __func__);
		rc = SLURM_ERROR;
		goto fini;
	}

	j = json_tokener_parse(resp_msg);
	if (j == NULL) {
		error("%s: json parser failed on %s", __func__, resp_msg);
		xfree(resp_msg);
		rc = SLURM_ERROR;
		goto fini;
	}
	xfree(resp_msg);
	json_object_object_foreachC(j, iter) {
		if (xstrcmp(iter.key, "nids"))
			continue;
		mcdram_cap = _json_parse_mcdram_cap_array(j, iter.key,
							  &mcdram_cap_cnt);
		break;
	}
	json_object_put(j);	/* Frees json memory */

	/*
	 * Load current MCDRAM configuration
	 */
	script_argv = xmalloc(sizeof(char *) * 4);	/* NULL terminated */
	script_argv[0] = xstrdup("capmc");
	script_argv[1] = xstrdup("get_mcdram_cfg");
	START_TIMER;
	resp_msg = _run_script(capmc_path, script_argv, &status);
	END_TIMER;
	if (debug_flag)
		info("%s: get_mcdram_cfg ran for %s", __func__, TIME_STR);
	_log_script_argv(script_argv, resp_msg);
	_free_script_argv(script_argv);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		error("%s: get_mcdram_cfg status:%u response:%s",
		      __func__, status, resp_msg);
	}
	if (resp_msg == NULL) {
		info("%s: get_mcdram_cfg returned no information", __func__);
		rc = SLURM_ERROR;
		goto fini;
	}

	j = json_tokener_parse(resp_msg);
	if (j == NULL) {
		error("%s: json parser failed on %s", __func__, resp_msg);
		xfree(resp_msg);
		rc = SLURM_ERROR;
		goto fini;
	}
	xfree(resp_msg);
	json_object_object_foreachC(j, iter) {
		if (xstrcmp(iter.key, "nids"))
			continue;
		mcdram_cfg = _json_parse_mcdram_cfg_array(j, iter.key,
							  &mcdram_cfg_cnt);
		break;
	}
	json_object_put(j);	/* Frees json memory */

	mcdram_cfg2 = _load_current_mcdram(&mcdram_cfg2_cnt);

	/*
	 * Load available NUMA capabilities
	 */
	script_argv = xmalloc(sizeof(char *) * 4);	/* NULL terminated */
	script_argv[0] = xstrdup("capmc");
	script_argv[1] = xstrdup("get_numa_capabilities");
	START_TIMER;
	resp_msg = _run_script(capmc_path, script_argv, &status);
	END_TIMER;
	if (debug_flag) {
		info("%s: get_numa_capabilities ran for %s",
		     __func__, TIME_STR);
	}
	_log_script_argv(script_argv, resp_msg);
	_free_script_argv(script_argv);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		error("%s: get_numa_capabilities status:%u response:%s",
		      __func__, status, resp_msg);
	}
	if (resp_msg == NULL) {
		info("%s: get_numa_capabilities returned no information",
		     __func__);
		rc = SLURM_ERROR;
		goto fini;
	}

	j = json_tokener_parse(resp_msg);
	if (j == NULL) {
		error("%s: json parser failed on %s", __func__, resp_msg);
		xfree(resp_msg);
		rc = SLURM_ERROR;
		goto fini;
	}
	xfree(resp_msg);
	json_object_object_foreachC(j, iter) {
		if (xstrcmp(iter.key, "nids"))
			continue;
		numa_cap = _json_parse_numa_cap_array(j, iter.key,
						      &numa_cap_cnt);
		break;
	}
	json_object_put(j);	/* Frees json memory */

	/*
	 * Load current NUMA configuration
	 */
	script_argv = xmalloc(sizeof(char *) * 4);	/* NULL terminated */
	script_argv[0] = xstrdup("capmc");
	script_argv[1] = xstrdup("get_numa_cfg");
	START_TIMER;
	resp_msg = _run_script(capmc_path, script_argv, &status);
	END_TIMER;
	if (debug_flag)
		info("%s: get_numa_cfg ran for %s", __func__, TIME_STR);
	_log_script_argv(script_argv, resp_msg);
	_free_script_argv(script_argv);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		error("%s: get_numa_cfg status:%u response:%s",
		      __func__, status, resp_msg);
	}
	if (resp_msg == NULL) {
		info("%s: get_numa_cfg returned no information", __func__);
		rc = SLURM_ERROR;
		goto fini;
	}

	j = json_tokener_parse(resp_msg);
	if (j == NULL) {
		error("%s: json parser failed on %s", __func__, resp_msg);
		xfree(resp_msg);
		rc = SLURM_ERROR;
		goto fini;
	}
	xfree(resp_msg);
	json_object_object_foreachC(j, iter) {
		if (xstrcmp(iter.key, "nids"))
			continue;
		numa_cfg = _json_parse_numa_cfg_array(j, iter.key,
						      &numa_cfg_cnt);
		break;
	}
	json_object_put(j);	/* Frees json memory */

	numa_cfg2 = _load_current_numa(&numa_cfg2_cnt);

	if (debug_flag) {
		_mcdram_cap_log(mcdram_cap, mcdram_cap_cnt);
		_mcdram_cfg_log(mcdram_cfg, mcdram_cfg_cnt);
		_mcdram_cfg2_log(mcdram_cfg2, mcdram_cfg2_cnt);
		_numa_cap_log(numa_cap, numa_cap_cnt);
		_numa_cfg_log(numa_cfg, numa_cfg_cnt);
		_numa_cfg2_log(numa_cfg2, numa_cfg2_cnt);
	}
	for (i = 0; i < mcdram_cfg_cnt; i++) {
		for (k = 0; k < mcdram_cfg2_cnt; k++) {
			if (!mcdram_cfg2[k].node_bitmap ||
			    !bit_test(mcdram_cfg2[k].node_bitmap,
				      mcdram_cfg[i].nid))
				continue;
			if (mcdram_cfg[i].mcdram_pct !=
			    mcdram_cfg2[k].hbm_pct) {
				debug("%s: HBM mismatch between capmc and cnselect for nid %u (%u != %d)",
				      __func__, mcdram_cfg[i].nid,
				      mcdram_cfg[i].mcdram_pct,
				      mcdram_cfg2[k].hbm_pct);
				mcdram_cfg[i].mcdram_pct=mcdram_cfg2[k].hbm_pct;
				xfree(mcdram_cfg[i].mcdram_cfg);
				mcdram_cfg[i].mcdram_cfg =
					xstrdup(mcdram_cfg2[k].mcdram_cfg);
			}
			break;
		}
	}
	for (i = 0; i < numa_cfg_cnt; i++) {
		for (k = 0; k < numa_cfg2_cnt; k++) {
			if (!numa_cfg2[k].node_bitmap ||
			    !bit_test(numa_cfg2[k].node_bitmap,
				      numa_cfg[i].nid))
				continue;
			if (xstrcmp(numa_cfg[i].numa_cfg,
				    numa_cfg2[k].numa_cfg)) {
				debug("%s: NUMA mismatch between capmc and cnselect for nid %u (%s != %s)",
				      __func__, numa_cfg[i].nid,
				      numa_cfg[i].numa_cfg,
				      numa_cfg2[k].numa_cfg);
				xfree(numa_cfg[i].numa_cfg);
				numa_cfg[i].numa_cfg =
					xstrdup(numa_cfg2[k].numa_cfg);
			}
			break;
		}
	}

	START_TIMER;
	if (node_list) {
		if ((host_list = hostlist_create(node_list)) == NULL) {
			error ("hostlist_create error on %s: %m", node_list);
			goto fini;
		}
		while ((node_name = hostlist_shift(host_list))) {
			node_ptr = find_node_record(node_name);
			if (node_ptr) {
				_update_node_features(node_ptr,
						      mcdram_cap,mcdram_cap_cnt,
						      mcdram_cfg,mcdram_cfg_cnt,
						      numa_cap, numa_cap_cnt,
						      numa_cfg, numa_cfg_cnt);
			}
			xfree(node_name);
		}
		hostlist_destroy (host_list);
	} else {
		for (i=0, node_ptr=node_record_table_ptr; i<node_record_count;
		     i++, node_ptr++) {
			xfree(node_ptr->features_act);
			_strip_knl_opts(&node_ptr->features);
			if (node_ptr->features && !node_ptr->features_act) {
				node_ptr->features_act =
					xstrdup(node_ptr->features);
			} else {
				_strip_knl_opts(&node_ptr->features_act);
			}
		}
		_update_all_node_features(mcdram_cap, mcdram_cap_cnt,
					  mcdram_cfg, mcdram_cfg_cnt,
					  numa_cap, numa_cap_cnt,
					  numa_cfg, numa_cfg_cnt);
	}
	END_TIMER;
	if (debug_flag)
		info("%s: update_node_features ran for %s", __func__, TIME_STR);

	last_node_update = time(NULL);

fini:	_mcdram_cap_free(mcdram_cap, mcdram_cap_cnt);
	_mcdram_cfg_free(mcdram_cfg, mcdram_cfg_cnt);
	_mcdram_cfg2_free(mcdram_cfg2, mcdram_cfg2_cnt);
	_numa_cap_free(numa_cap, numa_cap_cnt);
	_numa_cfg_free(numa_cfg, numa_cfg_cnt);
	_numa_cfg2_free(numa_cfg2, numa_cfg2_cnt);

	return rc;
}

/* Get this node's current and available MCDRAM and NUMA settings from BIOS.
 * avail_modes IN/OUT - append available modes, must be xfreed
 * current_mode IN/OUT - append current modes, must be xfreed
 *
 * NOTE: Not applicable on Cray systems; can be used on other systems.
 *
 * NOTES about syscfg (from Intel):
 * To display the BIOS Parameters:
 * >> syscfg /d biossettings <"BIOS variable Name">
 *
 * To Set the BIOS Parameters:
 * >> syscfg /bcs <AdminPw> <"BIOS variable Name"> <Value>
 * Note: If AdminPw is not set use ""
 */
extern void node_features_p_node_state(char **avail_modes, char **current_mode)
{
#if 0
	char *avail_states = NULL, *cur_state = NULL;
	char *resp_msg, *argv[10], *avail_sep = "", *cur_sep = "", *tok;
	int status = 0;

	if (!syscfg_path || !avail_modes || !current_mode)
		return;

	argv[0] = "syscfg";
	argv[1] = "/d";
	argv[2] = "BIOSSETTINGS";
	argv[3] = "Cluster Mode";
	argv[4] = NULL;
	resp_msg = _run_script(syscfg_path, argv, &status);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		error("%s: syscfg status:%u response:%s",
		      __func__, status, resp_msg);
	}
	if (resp_msg == NULL) {
		info("%s: syscfg returned no information", __func__);
	} else {
		tok = strstr(resp_msg, "Current Value : ");
		if (tok) {
			tok += 16;
			if (!strncasecmp(tok, "All2All", 3)) {
				cur_state = xstrdup("a2a");
				cur_sep = ",";
			} else if (!strncasecmp(tok, "Hemisphere", 3)) {
				cur_state = xstrdup("hemi");
				cur_sep = ",";
			} else if (!strncasecmp(tok, "Quadrant", 3)) {
				cur_state = xstrdup("quad");
				cur_sep = ",";
			} else if (!strncasecmp(tok, "SNC-2", 5)) {
				cur_state = xstrdup("snc2");
				cur_sep = ",";
			} else if (!strncasecmp(tok, "SNC-4", 5)) {
				cur_state = xstrdup("snc4");
				cur_sep = ",";
			}
		}
		if (xstrcasestr(resp_msg, "All2All")) {
			xstrfmtcat(avail_states, "%s%s", avail_sep, "a2a");
			avail_sep = ",";
		}
		if (xstrcasestr(resp_msg, "Hemisphere")) {
			xstrfmtcat(avail_states, "%s%s", avail_sep, "hemi");
			avail_sep = ",";
		}
		if (xstrcasestr(resp_msg, "Quadrant")) {
			xstrfmtcat(avail_states, "%s%s", avail_sep, "quad");
			avail_sep = ",";
		}
		if (xstrcasestr(resp_msg, "SNC-2")) {
			xstrfmtcat(avail_states, "%s%s", avail_sep, "snc2");
			avail_sep = ",";
		}
		if (xstrcasestr(resp_msg, "SNC-4")) {
			xstrfmtcat(avail_states, "%s%s", avail_sep, "snc4");
			avail_sep = ",";
		}
		xfree(resp_msg);
	}

	argv[0] = "syscfg";
	argv[1] = "/d";
	argv[2] = "BIOSSETTINGS";
	argv[3] = "Memory Mode";
	argv[4] = NULL;
	resp_msg = _run_script(syscfg_path, argv, &status);
	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
		error("%s: syscfg status:%u response:%s",
		      __func__, status, resp_msg);
	}
	if (resp_msg == NULL) {
		info("%s: syscfg returned no information", __func__);
	} else {
		tok = strstr(resp_msg, "Current Value : ");
		if (tok) {
			tok += 16;
			if (!strncasecmp(tok, "Cache", 3)) {
				xstrfmtcat(cur_state, "%s%s", cur_sep, "cache");
			} else if (!strncasecmp(tok, "Flat", 3)) {
				xstrfmtcat(cur_state, "%s%s", cur_sep, "flat");
			} else if (!strncasecmp(tok, "Hybrid", 3)) {
				xstrfmtcat(cur_state, "%s%s", cur_sep, "equal");
			}
		}
		if (xstrcasestr(resp_msg, "Cache")) {
			xstrfmtcat(avail_states, "%s%s", avail_sep, "cache");
			avail_sep = ",";
		}
		if (xstrcasestr(resp_msg, "Flat")) {
			xstrfmtcat(avail_states, "%s%s", avail_sep, "flat");
			avail_sep = ",";
		}
		if (xstrcasestr(resp_msg, "Hybrid")) {
			xstrfmtcat(avail_states, "%s%s", avail_sep, "equal");
			/* avail_sep = ",";	CLANG error: Dead assignment */
		}
		xfree(resp_msg);
	}

	if (*avail_modes) {	/* Append for multiple node_features plugins */
		if (*avail_modes[0])
			avail_sep = ",";
		else
			avail_sep = "";
		xstrfmtcat(*avail_modes, "%s%s", avail_sep, avail_states);
		xfree(avail_states);
	} else {
		*avail_modes = avail_states;
	}

	if (*current_mode) {	/* Append for multiple node_features plugins */
		if (*current_mode[0])
			cur_sep = ",";
		else
			cur_sep = "";
		xstrfmtcat(*current_mode, "%s%s", cur_sep, cur_state);
		xfree(cur_state);
	} else {
		*current_mode = cur_state;
	}
#endif
}

/* Test if a job's feature specification is valid */
extern int node_features_p_job_valid(char *job_features)
{
	uint16_t job_mcdram, job_numa;
	int mcdram_cnt, numa_cnt;

	if ((job_features == NULL) || (job_features[0] == '\0'))
		return SLURM_SUCCESS;

	if (strchr(job_features, '[') ||	/* Unsupported operator */
	    strchr(job_features, ']') ||
	    strchr(job_features, '|') ||
	    strchr(job_features, '*'))
		return ESLURM_INVALID_KNL;
	
	job_mcdram = _knl_mcdram_parse(job_features, "&,");
	mcdram_cnt = _knl_mcdram_bits_cnt(job_mcdram);
	if (mcdram_cnt > 1)			/* Multiple MCDRAM options */
		return ESLURM_INVALID_KNL;

	job_numa = _knl_numa_parse(job_features, "&,");
	numa_cnt = _knl_numa_bits_cnt(job_numa);
	if (numa_cnt > 1)			/* Multiple NUMA options */
		return ESLURM_INVALID_KNL;

	/* snc4 only allowed with cache today due to invalid config information
	 * reported by kernel to hwloc, then to Slurm */
	if (!job_numa) {
	    job_numa = default_numa;
	}
	if (!job_mcdram) {
	    job_mcdram = default_mcdram;
	}
	if (job_numa == KNL_SNC4 && job_mcdram != KNL_CACHE) {
		return ESLURM_INVALID_KNL;
	}

	return SLURM_SUCCESS;
}

/* Translate a job's feature request to the node features needed at boot time */
extern char *node_features_p_job_xlate(char *job_features)
{
	char *node_features = NULL;
	char *tmp, *save_ptr = NULL, *sep = "", *tok;
	bool has_numa = false, has_mcdram = false;

	if ((job_features == NULL) || (job_features[0] ==  '\0'))
		return node_features;

	tmp = xstrdup(job_features);
	tok = strtok_r(tmp, "&", &save_ptr);
	while (tok) {
		bool knl_opt = false;
		if (_knl_mcdram_token(tok)) {
			if (!has_mcdram) {
				has_mcdram = true;
				knl_opt = true;
			}
		}
		if (_knl_numa_token(tok)) {
			if (!has_numa) {
				has_numa = true;
				knl_opt = true;
			}
		}
		if (knl_opt) {
			xstrfmtcat(node_features, "%s%s", sep, tok);
			sep = ",";
		}
		tok = strtok_r(NULL, "&", &save_ptr);
	}
	xfree(tmp);

	/* Add default options */
	if (!has_mcdram) {
		tmp = _knl_mcdram_str(default_mcdram);
		xstrfmtcat(node_features, "%s%s", sep, tmp);
		sep = ",";
		xfree(tmp);
	}
	if (!has_numa) {
		tmp = _knl_numa_str(default_numa);
		xstrfmtcat(node_features, "%s%s", sep, tmp);
		// sep = ",";		Removed to avoid CLANG error
		xfree(tmp);
	}

	return node_features;
}

/* Return true if the plugin requires PowerSave mode for booting nodes */
extern bool node_features_p_node_power(void)
{
	return true;
}

/* Return true if the plugin requires RebootProgram for booting nodes */
extern bool node_features_p_node_reboot(void)
{
	return false;
}

/* Note the active features associated with a set of nodes have been updated.
 * Specifically update the node's "hbm" GRES value as needed.
 * IN active_features - New active features
 * IN node_bitmap - bitmap of nodes changed
 * RET error code */
extern int node_features_p_node_update(char *active_features,
				       bitstr_t *node_bitmap)
{
	int i, i_first, i_last;
	int rc = SLURM_SUCCESS;
	uint16_t mcdram_inx;
	uint64_t mcdram_size;
	struct node_record *node_ptr;

	if (mcdram_per_node == NULL) {
		error("%s: mcdram_per_node == NULL", __func__);
		return SLURM_ERROR;
	}
	mcdram_inx = _knl_mcdram_parse(active_features, ",");
	if (mcdram_inx == 0)
		return rc;
	for (i = 0; i < KNL_MCDRAM_CNT; i++) {
		if ((KNL_CACHE << i) == mcdram_inx)
			break;
	}
	if ((i >= KNL_MCDRAM_CNT) || (mcdram_pct[i] == -1))
		return rc;
	mcdram_inx = i;

	xassert(node_bitmap);
	i_first = bit_ffs(node_bitmap);
	if (i_first >= 0)
		i_last = bit_fls(node_bitmap);
	else
		i_last = i_first - 1;
	for (i = i_first; i <= i_last; i++) {
		if (!bit_test(node_bitmap, i))
			continue;
		if (i >= node_record_count) {
			error("%s: Invalid node index (%d >= %d)",
			      __func__, i, node_record_count);
			rc = SLURM_ERROR;
			break;
		}
		mcdram_size = mcdram_per_node[i] *
			      (100 - mcdram_pct[mcdram_inx]) / 100;
		node_ptr = node_record_table_ptr + i;
		gres_plugin_node_feature(node_ptr->name, "hbm",
					 mcdram_size, &node_ptr->gres,
					 &node_ptr->gres_list);
	}

	return rc;
}

/* Translate a node's feature specification by replacing any features associated
 * with this plugin in the original value with the new values, preserving any
 * features that are not associated with this plugin
 * RET node's new merged features, must be xfreed */
extern char *node_features_p_node_xlate(char *new_features, char *orig_features)
{
	char *node_features = NULL;
	char *tmp, *save_ptr = NULL, *sep = "", *tok;

	if (new_features) {
		tmp = xstrdup(new_features);
		tok = strtok_r(tmp, ",", &save_ptr);
		while (tok) {
			if ((_knl_mcdram_token(tok) != 0) ||
			    (_knl_numa_token(tok)   != 0)) {
				xstrfmtcat(node_features, "%s%s", sep, tok);
				sep = ",";
			}
			tok = strtok_r(NULL, ",", &save_ptr);
		}
		xfree(tmp);
	}

	if (!node_features) {	/* No new info from compute node */
		node_features = xstrdup(orig_features);
		return node_features;
	}

	tmp = xstrdup(orig_features);
	tok = strtok_r(tmp, ",", &save_ptr);
	while (tok) {
		if ((_knl_mcdram_token(tok) == 0) &&
		    (_knl_numa_token(tok)   == 0)) {
			xstrfmtcat(node_features, "%s%s", sep, tok);
			sep = ",";
		}
		tok = strtok_r(NULL, ",", &save_ptr);
	}
	xfree(tmp);

	return node_features;
}

/* Determine if the specified user can modify the currently available node
 * features */
extern bool node_features_p_user_update(uid_t uid)
{
	int i;

	if (allowed_uid_cnt == 0)   /* Default is ALL users allowed to update */
		return true;

	for (i = 0; i < allowed_uid_cnt; i++) {
		if (allowed_uid[i] == uid)
			return true;
	}

	return false;
}

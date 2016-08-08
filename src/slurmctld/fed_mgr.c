/*****************************************************************************\
 *  fed_mgr.c - functions for federations
 *****************************************************************************
 *  Copyright (C) 2016 SchedMD LLC.
 *  Written by Brian Christiansen <brian@schedmd.com>
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

#include <pthread.h>

#if HAVE_SYS_PRCTL_H
#  include <sys/prctl.h>
#endif

#include "src/common/list.h"
#include "src/common/macros.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/slurmdbd_defs.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"
#include "src/slurmctld/fed_mgr.h"
#include "src/slurmctld/locks.h"
#include "src/slurmctld/slurmctld.h"
#include "src/slurmdbd/read_config.h"

#define FED_MGR_STATE_FILE       "fed_mgr_state"
#define FED_MGR_CLUSTER_ID_BEGIN 26

static char                  *fed_mgr_cluster_name = NULL;
static fed_elem_t             fed_mgr_fed_info;
static List                   fed_mgr_siblings     = NULL;
static slurmdb_cluster_rec_t *fed_mgr_loc_cluster  = NULL;

static pthread_t ping_thread  = 0;
static bool      stop_pinging = false;

static int _close_controller_conn(slurmdb_cluster_rec_t *conn)
{
	int rc = SLURM_SUCCESS;
	xassert(conn);
	slurm_mutex_lock(&conn->lock);
	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("closing sibling conn to %s", conn->name);

	if (conn->sockfd >= 0)
		rc = slurm_close_persist_controller_conn(conn->sockfd);
	conn->sockfd = -1;

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("closed sibling conn to %s", conn->name);
	slurm_mutex_unlock(&conn->lock);

	return rc;
}

static int _open_controller_conn(slurmdb_cluster_rec_t *conn)
{
	slurm_mutex_lock(&conn->lock);
	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("opening sibling conn to %s", conn->name);

	if (conn->control_host && conn->control_host[0] == '\0')
		conn->sockfd = -1;
	else
		conn->sockfd =
			slurm_open_persist_controller_conn(conn->control_host,
							   conn->control_port);

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("openend sibling conn to %s:%d", conn->name, conn->sockfd);
	slurm_mutex_unlock(&conn->lock);

	return conn->sockfd;
}

static int _send_recv_msg(slurmdb_cluster_rec_t *conn, slurm_msg_t *req,
			  slurm_msg_t *resp)
{
	int rc;
	slurm_mutex_lock(&conn->lock);
	rc = slurm_send_recv_msg(conn->sockfd, req, resp, 0);
	slurm_mutex_unlock(&conn->lock);

	return rc;
}

static int _ping_controller(slurmdb_cluster_rec_t *conn)
{
	int rc = SLURM_SUCCESS;
	slurm_msg_t *req_msg;
	slurm_msg_t *resp_msg;

	req_msg  = xmalloc(sizeof(slurm_msg_t));
	resp_msg = xmalloc(sizeof(slurm_msg_t));

	slurm_msg_t_init(req_msg);
	slurm_msg_t_init(resp_msg);
	req_msg->msg_type = REQUEST_PING;

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("pinging %s(%s:%d)", conn->name, conn->control_host,
		     conn->control_port);

	if ((rc = _send_recv_msg(conn, req_msg, resp_msg))) {
		error("failed to ping %s(%s:%d)",
		      conn->name, conn->control_host, conn->control_port);
		slurm_mutex_lock(&conn->lock);
		conn->sockfd = -1;
		slurm_mutex_unlock(&conn->lock);
	} else if ((rc = slurm_get_return_code(resp_msg->msg_type,
					       resp_msg->data)))
		error("ping returned error from %s(%s:%d)",
		      conn->name, conn->control_host, conn->control_port);
	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("finished pinging %s(%s:%d)", conn->name,
		     conn->control_host, conn->control_port);

	slurm_free_msg(req_msg);
	slurm_free_msg(resp_msg);

	return rc;
}

/*
 * close all sibling conns
 * must lock before entering.
 */
static int _close_sibling_conns()
{
	ListIterator itr;
	slurmdb_cluster_rec_t *conn;

	if (!fed_mgr_siblings)
		goto fini;

	itr = list_iterator_create(fed_mgr_siblings);
	while ((conn = list_next(itr))) {
		_close_controller_conn(conn);
	}
	list_iterator_destroy(itr);

fini:
	return SLURM_SUCCESS;
}

static void *_ping_thread(void *arg)
{
	slurmctld_lock_t fed_read_lock = {
		NO_LOCK, NO_LOCK, NO_LOCK, NO_LOCK, READ_LOCK };

#if HAVE_SYS_PRCTL_H
	if (prctl(PR_SET_NAME, "fed_ping", NULL, NULL, NULL) < 0) {
		error("%s: cannot set my name to %s %m", __func__, "fed_ping");
	}
#endif
	while (!stop_pinging &&
	       !slurmctld_config.shutdown_time) {
		ListIterator itr;
		slurmdb_cluster_rec_t *conn;

		lock_slurmctld(fed_read_lock);
		if (!fed_mgr_siblings)
			goto next;

		itr = list_iterator_create(fed_mgr_siblings);

		while ((conn = list_next(itr))) {
			if (conn->sockfd == -1)
				_open_controller_conn(conn);
			if (conn->sockfd == -1)
				continue;
			_ping_controller(conn);
		}
		list_iterator_destroy(itr);

next:
		unlock_slurmctld(fed_read_lock);

		sleep(5);
	}

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("Exiting ping thread");

	return NULL;
}

static void _create_ping_thread()
{
	pthread_attr_t attr;
	slurm_attr_init(&attr);
	stop_pinging = false;
	if (!ping_thread &&
	    (pthread_create(&ping_thread, &attr, _ping_thread, NULL) != 0)) {
		error("pthread_create of message thread: %m");
		slurm_attr_destroy(&attr);
		ping_thread = 0;
		return;
	}
	slurm_attr_destroy(&attr);
}

static void _destroy_ping_thread()
{
	stop_pinging = true;
	if (ping_thread) {
		/* can't wait for ping_thread to finish because it might be
		 * holding the read lock and we are already in the write lock.
		 * pthread_join(ping_thread, NULL);
		 */
		ping_thread = 0;
	}
}

/*
 * Must have FED write lock prior to entering
 */
static void _join_federation(slurmdb_cluster_rec_t *db_cluster, List siblings)
{
	slurmdb_cluster_rec_t *tmp_cluster;
	ListIterator c_itr;

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("Joining federation %s", db_cluster->fed.name);

	xfree(fed_mgr_fed_info.name);
	memcpy(&fed_mgr_fed_info, &db_cluster->fed, sizeof(fed_elem_t));
	fed_mgr_fed_info.name = xstrdup(db_cluster->fed.name);

	/* Store the cluster_rec from the db for passing this information back
	 * in scontrol show fed. This is so that it doesn't need to be stored in
	 * the siblings list. */
	slurmdb_destroy_cluster_rec(fed_mgr_loc_cluster);
	fed_mgr_loc_cluster = xmalloc(sizeof(slurmdb_cluster_rec_t));
	slurmdb_init_cluster_rec(fed_mgr_loc_cluster, false);
	slurmdb_copy_cluster_rec(fed_mgr_loc_cluster, db_cluster);

	/* add clusters from federation into local list */
	if (fed_mgr_siblings) {
		/* close connections to all other siblings */
		/* free sib list as host and ports may have changed */
		_close_sibling_conns();
		FREE_NULL_LIST(fed_mgr_siblings);
	}

	fed_mgr_siblings = list_create(slurmdb_destroy_cluster_rec);
	c_itr = list_iterator_create(siblings);
	while ((tmp_cluster = list_next(c_itr))) {
		slurmdb_cluster_rec_t *sibling;

		if (!xstrcmp(tmp_cluster->name, fed_mgr_cluster_name))
			continue;

		sibling = xmalloc(sizeof(slurmdb_cluster_rec_t));
		slurmdb_init_cluster_rec(sibling, false);
		slurmdb_copy_cluster_rec(sibling, tmp_cluster);

		_open_controller_conn(sibling);

		list_append(fed_mgr_siblings, sibling);
	}
	list_iterator_destroy(c_itr);

	_create_ping_thread();
}

/*
 * Must have FED write lock prior to entering
 */
static void _leave_federation()
{
	if (!fed_mgr_fed_info.name)
		return;

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("Leaving federation %s", fed_mgr_fed_info.name);

	_close_sibling_conns();
	_destroy_ping_thread();
	xfree(fed_mgr_fed_info.name);
	memset(&fed_mgr_fed_info, 0, sizeof(fed_mgr_fed_info));
	FREE_NULL_LIST(fed_mgr_siblings);
	slurmdb_destroy_cluster_rec(fed_mgr_loc_cluster);
	fed_mgr_loc_cluster = NULL;
}

extern int fed_mgr_init()
{
	slurmctld_lock_t fed_write_lock = {
		NO_LOCK, NO_LOCK, NO_LOCK, NO_LOCK, WRITE_LOCK };

	lock_slurmctld(fed_write_lock);

	if (!fed_mgr_cluster_name)
		fed_mgr_cluster_name = slurm_get_cluster_name();

	unlock_slurmctld(fed_write_lock);

	return SLURM_SUCCESS;
}

extern int fed_mgr_fini()
{
	slurmctld_lock_t fed_write_lock = {
		NO_LOCK, NO_LOCK, NO_LOCK, NO_LOCK, WRITE_LOCK };

	lock_slurmctld(fed_write_lock);

	xfree(fed_mgr_cluster_name);
	_leave_federation();

	unlock_slurmctld(fed_write_lock);

	return SLURM_SUCCESS;
}

extern int fed_mgr_update_feds(slurmdb_update_object_t *update)
{
	List feds;
	ListIterator f_itr;
	slurmdb_federation_rec_t *fed = NULL;
	bool part_of_fed = false;
	slurmctld_lock_t fed_write_lock = {
		NO_LOCK, NO_LOCK, NO_LOCK, NO_LOCK, WRITE_LOCK };

	if (!update->objects)
		return SLURM_SUCCESS;

	if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
		info("Got FEDS");

	feds = update->objects;
	f_itr = list_iterator_create(feds);

	fed_mgr_init();

	lock_slurmctld(fed_write_lock);

	/* find the federation that this cluster is in.
	 * if it's changed from last time then update stored information.
	 * grab other clusters in federation
	 * establish connections with each cluster in federation */

	/* what if a remote cluster is removed from federation.
	 * have to detect that and close the connection to the remote */
	while ((fed = list_next(f_itr))) {
		ListIterator c_itr = list_iterator_create(fed->cluster_list);
		slurmdb_cluster_rec_t *cluster = NULL;

		if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
			info("Fed:%s Clusters:%d", fed->name,
			     list_count(fed->cluster_list));
		while ((cluster = list_next(c_itr))) {
			if (!xstrcasecmp(cluster->name, fed_mgr_cluster_name)) {
				part_of_fed = true;
				_join_federation(cluster, fed->cluster_list);
				break;
			}
		}
		list_iterator_destroy(c_itr);

		if (part_of_fed)
			break;
	}
	list_iterator_destroy(f_itr);

	if (!part_of_fed) {
		if (slurmctld_conf.debug_flags & DEBUG_FLAG_FEDR)
			info("Not part of any federation");
		_leave_federation();
	}

	unlock_slurmctld(fed_write_lock);

	return SLURM_SUCCESS;
}

extern int fed_mgr_get_fed_info(slurmdb_federation_rec_t **ret_fed)
{
	slurmdb_federation_rec_t *out_fed;
	slurmctld_lock_t fed_read_lock = {
		NO_LOCK, NO_LOCK, NO_LOCK, NO_LOCK, READ_LOCK };

	xassert(ret_fed);

	out_fed = (slurmdb_federation_rec_t *)
		xmalloc(sizeof(slurmdb_federation_rec_t));
	slurmdb_init_federation_rec(out_fed, false);

	lock_slurmctld(fed_read_lock);

	if (fed_mgr_fed_info.name) {
		slurmdb_federation_rec_t tmp_fed;
		slurmdb_init_federation_rec(&tmp_fed, false);

		tmp_fed.name         = fed_mgr_fed_info.name;
		tmp_fed.cluster_list = fed_mgr_siblings;

		slurmdb_copy_federation_rec(out_fed, &tmp_fed);

		/* add local cluster info */
		slurmdb_cluster_rec_t *tmp_cluster;
		tmp_cluster = xmalloc(sizeof(slurmdb_cluster_rec_t));
		slurmdb_init_cluster_rec(tmp_cluster, false);
		slurmdb_copy_cluster_rec(tmp_cluster, fed_mgr_loc_cluster);
		list_append(out_fed->cluster_list, tmp_cluster);
	}

	unlock_slurmctld(fed_read_lock);

	*ret_fed = out_fed;

	return SLURM_SUCCESS;
}


/* Make copy of fed_mgr_siblings and add the fed_mgr_loc_cluster to the list to
 * save. fed_mgr_state_load will separate out the local cluster from the
 * siblings.
 */
static List _make_state_save_siblings()
{
	List ret_list = NULL;
	ListIterator itr;
	slurmdb_cluster_rec_t *tmp_rec;
	slurmdb_cluster_rec_t *tmp_fed_mgr_loc_cluster;
	slurmctld_lock_t fed_read_lock = {
		NO_LOCK, NO_LOCK, NO_LOCK, NO_LOCK, READ_LOCK };

	lock_slurmctld(fed_read_lock);

	if (!fed_mgr_fed_info.name)
		goto end_it;

	ret_list = list_create(slurmdb_destroy_cluster_rec);

	/* local cluster */
	tmp_fed_mgr_loc_cluster = xmalloc(sizeof(slurmdb_cluster_rec_t));
	slurmdb_init_cluster_rec(tmp_fed_mgr_loc_cluster, false);
	slurmdb_copy_cluster_rec(tmp_fed_mgr_loc_cluster, fed_mgr_loc_cluster);
	list_append(ret_list, tmp_fed_mgr_loc_cluster);

	/* siblings */
	itr = list_iterator_create(fed_mgr_siblings);
	while ((tmp_rec = list_next(itr))) {
		slurmdb_cluster_rec_t *new_rec;
		new_rec = xmalloc(sizeof(slurmdb_cluster_rec_t));
		slurmdb_init_cluster_rec(new_rec, false);
		slurmdb_copy_cluster_rec(new_rec, tmp_rec);
		list_append(ret_list, new_rec);
	}
	list_iterator_destroy(itr);

end_it:
	unlock_slurmctld(fed_read_lock);
	return ret_list;
}

extern int fed_mgr_state_save(char *state_save_location)
{
	int error_code = 0, log_fd;
	char *old_file = NULL, *new_file = NULL, *reg_file = NULL;
	dbd_list_msg_t msg;
	Buf buffer = init_buf(0);

	DEF_TIMERS;

	START_TIMER;

	/* write header: version, time */
	pack16(SLURM_PROTOCOL_VERSION, buffer);
	pack_time(time(NULL), buffer);

	memset(&msg, 0, sizeof(dbd_list_msg_t));

	msg.my_list = _make_state_save_siblings();
	slurmdbd_pack_list_msg(&msg, SLURM_PROTOCOL_VERSION,
			       DBD_ADD_CLUSTERS, buffer);
	FREE_NULL_LIST(msg.my_list);

	/* write the buffer to file */
	reg_file = xstrdup_printf("%s/%s", state_save_location,
				  FED_MGR_STATE_FILE);
	old_file = xstrdup_printf("%s.old", reg_file);
	new_file = xstrdup_printf("%s.new", reg_file);

	log_fd = creat(new_file, 0600);
	if (log_fd < 0) {
		error("Can't save state, create file %s error %m", new_file);
		error_code = errno;
	} else {
		int pos = 0, nwrite = get_buf_offset(buffer), amount;
		char *data = (char *)get_buf_data(buffer);
		while (nwrite > 0) {
			amount = write(log_fd, &data[pos], nwrite);
			if ((amount < 0) && (errno != EINTR)) {
				error("Error writing file %s, %m", new_file);
				error_code = errno;
				break;
			}
			nwrite -= amount;
			pos    += amount;
		}
		fsync(log_fd);
		close(log_fd);
	}
	if (error_code)
		(void) unlink(new_file);
	else {			/* file shuffle */
		(void) unlink(old_file);
		if (link(reg_file, old_file))
			debug4("unable to create link for %s -> %s: %m",
			       reg_file, old_file);
		(void) unlink(reg_file);
		if (link(new_file, reg_file))
			debug4("unable to create link for %s -> %s: %m",
			       new_file, reg_file);
		(void) unlink(new_file);
	}
	xfree(old_file);
	xfree(reg_file);
	xfree(new_file);

	free_buf(buffer);

	END_TIMER2("fed_mgr_state_save");

	return error_code;
}

extern int fed_mgr_state_load(char *state_save_location)
{
	Buf buffer = NULL;
	dbd_list_msg_t *msg = NULL;
	char *data = NULL, *state_file;
	time_t buf_time;
	uint16_t ver = 0;
	uint32_t data_size = 0;
	int state_fd;
	int data_allocated, data_read = 0, error_code = SLURM_SUCCESS;
	slurmdb_cluster_rec_t *cluster = NULL;
	slurmctld_lock_t fed_write_lock = {
		NO_LOCK, NO_LOCK, NO_LOCK, NO_LOCK, WRITE_LOCK };

	state_file = xstrdup_printf("%s/%s", state_save_location,
				    FED_MGR_STATE_FILE);
	state_fd = open(state_file, O_RDONLY);
	if (state_fd < 0) {
		error("No fed_mgr state file (%s) to recover", state_file);
		xfree(state_file);
		return SLURM_SUCCESS;
	} else {
		data_allocated = BUF_SIZE;
		data = xmalloc(data_allocated);
		while (1) {
			data_read = read(state_fd, &data[data_size],
					 BUF_SIZE);
			if (data_read < 0) {
				if (errno == EINTR)
					continue;
				else {
					error("Read error on %s: %m",
					      state_file);
					break;
				}
			} else if (data_read == 0)	/* eof */
				break;
			data_size      += data_read;
			data_allocated += data_read;
			xrealloc(data, data_allocated);
		}
		close(state_fd);
	}
	xfree(state_file);

	buffer = create_buf(data, data_size);

	safe_unpack16(&ver, buffer);

	debug3("Version in fed_mgr_state header is %u", ver);
	if (ver > SLURM_PROTOCOL_VERSION || ver < SLURM_MIN_PROTOCOL_VERSION) {
		error("***********************************************");
		error("Can not recover fed_mgr state, incompatible version, "
		      "got %u need > %u <= %u", ver,
		      SLURM_MIN_PROTOCOL_VERSION, SLURM_PROTOCOL_VERSION);
		error("***********************************************");
		free_buf(buffer);
		return EFAULT;
	}

	safe_unpack_time(&buf_time, buffer);

	error_code = slurmdbd_unpack_list_msg(&msg, ver, DBD_ADD_CLUSTERS,
					      buffer);
	if (error_code != SLURM_SUCCESS)
		goto unpack_error;
	else if (!msg->my_list) {
		error("No feds retrieved");
	}

	fed_mgr_init(); /* set fed_mgr_cluster_name */

	lock_slurmctld(fed_write_lock);

	if (msg->my_list &&
	    !(cluster = list_find_first(msg->my_list,
					slurmdb_find_cluster_in_list,
					fed_mgr_cluster_name))) {
		error("This cluster doesn't exist in the fed siblings");
		unlock_slurmctld(fed_write_lock);
		slurmdbd_free_list_msg(msg);
		goto unpack_error;
	} else if (cluster) {
		_join_federation(cluster, msg->my_list);
	}
	unlock_slurmctld(fed_write_lock);

	slurmdbd_free_list_msg(msg);
	free_buf(buffer);

	return SLURM_SUCCESS;

unpack_error:
	free_buf(buffer);

	return SLURM_ERROR;
}

extern int _find_sibling_by_ip(void *x, void *key)
{
	slurmdb_cluster_rec_t *object = (slurmdb_cluster_rec_t *)x;
	char *ip = (char *)key;

	if (!xstrcmp(object->control_host, ip))
		return 1;

	return 0;
}

extern char *fed_mgr_find_sibling_name_by_ip(char *ip)
{
	char *name = NULL;
	slurmdb_cluster_rec_t *sibling = NULL;
	slurmctld_lock_t fed_read_lock = {
		NO_LOCK, NO_LOCK, NO_LOCK, NO_LOCK, READ_LOCK };

	lock_slurmctld(fed_read_lock);
	if (fed_mgr_siblings &&
	    (sibling = list_find_first(fed_mgr_siblings, _find_sibling_by_ip,
				       ip)))
		name = xstrdup(sibling->name);
	unlock_slurmctld(fed_read_lock);

	return name;
}

/*
 * Returns true if the cluster is part of a federation.
 */
extern bool fed_mgr_is_active()
{
	int rc = false;
	slurmctld_lock_t fed_read_lock = {
		NO_LOCK, NO_LOCK, NO_LOCK, NO_LOCK, READ_LOCK };

	lock_slurmctld(fed_read_lock);
	if (fed_mgr_fed_info.name)
		rc = true;
	unlock_slurmctld(fed_read_lock);

	return rc;
}

/*
 * Returns federated job id (<local id> + <cluster id>.
 * Bits  0-25: Local job id
 * Bits 26-31: Cluster id
 */
extern uint32_t fed_mgr_get_job_id(uint32_t orig)
{
	return orig + (fed_mgr_fed_info.id << FED_MGR_CLUSTER_ID_BEGIN);
}

/*
 * Returns the local job id from a federated job id.
 */
extern uint32_t fed_mgr_get_local_id(uint32_t id)
{
	return id & MAX_JOB_ID;
}

/*
 * Returns the cluster id from a federated job id.
 */
extern uint32_t fed_mgr_get_cluster_id(uint32_t id)
{
	return id >> FED_MGR_CLUSTER_ID_BEGIN;
}

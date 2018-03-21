/*
 *   Copyright (C) 2018 Samsung Electronics Co., Ltd.
 *
 *   linux-cifsd-devel@sourceforge.net
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */
#include <memory.h>
#include <glib.h>
#include <errno.h>
/*
 * FIXME: should be in /include/linux/
 */
#include <cifsd_server.h>

#include <cifsdtools.h>
#include <worker_pool.h>
#include <ipc.h>

#include <management/user.h>
#include <management/share.h>
#include <management/tree_conn.h>

#define MAX_WORKER_THREADS	4
static GThreadPool *pool;

#define VALID_IPC_MSG(m,t) 					\
	({							\
		int ret = 1;					\
		if (((m)->sz != sizeof(t))) {			\
			pr_err("Bad message: %s\n", __func__);	\
			ret = 0;				\
		}						\
		ret;						\
	})

static int __login_request(struct cifsd_login_request *req,
			   struct cifsd_login_response *resp)
{
	struct cifsd_user *user;
	size_t hash_sz;

	user = usm_lookup_user(req->account);
	if (!user) {
		resp->status = CIFSD_LOGIN_STATUS_UNKNOWN_USER;
		return -EINVAL;
	}

	hash_sz = get_user_pass_sz(user);
	if (hash_sz > sizeof(resp->hash)) {
		put_cifsd_user(user);

		pr_err("User hash is too large: %d\n", hash_sz);
		return -E2BIG;
	}

	resp->status = CIFSD_LOGIN_STATUS_OK;
	resp->hash_sz = hash_sz;
	memcpy(resp->hash, get_user_passhash(user), hash_sz);
	put_cifsd_user(user);
}

static int login_request(struct ipc_msg *msg)
{
	struct cifsd_login_request *req;
	struct cifsd_login_response *resp;
	struct ipc_msg *resp_msg;

	resp_msg = ipc_msg_alloc(sizeof(*resp));
	if (!resp_msg)
		goto out;

	req = IPC_MSG_PAYLOAD(msg);
	resp = IPC_MSG_PAYLOAD(resp_msg);

	resp->status = CIFSD_LOGIN_STATUS_INVALID;
	if (VALID_IPC_MSG(msg, struct cifsd_login_request))
		__login_request(req, resp);

	resp_msg->destination = CIFSD_IPC_DESTINATION_KERNEL;
	resp_msg->type = CIFSD_EVENT_LOGIN_RESPONSE;
	resp->handle = req->handle;

	ipc_msg_send(resp_msg);
out:
	ipc_msg_free(resp_msg);
	return 0;
}

static int tree_connect_request(struct ipc_msg *msg)
{
	struct cifsd_tree_connect_request *req;
	struct cifsd_tree_connect_response *resp;
	struct ipc_msg *resp_msg;

	resp_msg = ipc_msg_alloc(sizeof(*resp));
	if (!resp_msg)
		goto out;

	req = IPC_MSG_PAYLOAD(msg);
	resp = IPC_MSG_PAYLOAD(resp_msg);

	resp->status = CIFSD_TREE_CONN_STATUS_ERROR;
	resp->connection_id = -1;
	resp->connection_flags = 0;

	if (VALID_IPC_MSG(msg, struct cifsd_tree_connect_request))
		tcm_handle_tree_connect(req, resp);

	resp_msg->destination = CIFSD_IPC_DESTINATION_KERNEL;
	resp_msg->type = CIFSD_EVENT_TREE_CONNECT_RESPONSE;
	resp->handle = req->handle;

	ipc_msg_send(resp_msg);
out:
	ipc_msg_free(resp_msg);
	return 0;
}

static int tree_disconnect_request(struct ipc_msg *msg)
{
	struct cifsd_tree_disconnect_request *req;

	if (!VALID_IPC_MSG(msg, struct cifsd_tree_disconnect_request))
		return -EINVAL;

	req = IPC_MSG_PAYLOAD(msg);
	tcm_handle_tree_disconnect(req->connect_id);

	return 0;
}

static int logout_request(struct ipc_msg *msg)
{
	if (!VALID_IPC_MSG(msg, struct cifsd_logout_request))
		return -EINVAL;

	return 0;
}

static void worker_pool_fn(gpointer event, gpointer user_data)
{
	struct ipc_msg *msg = (struct ipc_msg *)event;

	switch (msg->type) {
	case CIFSD_EVENT_LOGIN_REQUEST:
		login_request(msg);
		break;

	case CIFSD_EVENT_TREE_CONNECT_REQUEST:
		tree_connect_request(msg);
		break;

	case CIFSD_EVENT_TREE_DISCONNECT_REQUEST:
		tree_disconnect_request(msg);
		break;

	case CIFSD_EVENT_LOGOUT_REQUEST:
		logout_request(msg);
		break;

	default:
		pr_err("Unknown IPC message type: %d\n", msg->type);
		break;
	}

	ipc_msg_free(msg);
}

int wp_ipc_msg_push(struct ipc_msg *msg)
{
	return g_thread_pool_push(pool, msg, NULL);
}

void wp_final_release(void)
{
	g_thread_pool_free(pool, 1, 1);
}

int wp_init(void)
{
	GError *err;

	pool = g_thread_pool_new(worker_pool_fn,
				 NULL,
				 MAX_WORKER_THREADS,
				 0,
				 &err);
	if (!pool) {
		if (err) {
			pr_err("Can't create pool: %s\n", err->message);
			g_error_free(err);
		}
		goto out_error;
	}

	return 0;
out_error:
	wp_final_release();
	return -ENOMEM;
}
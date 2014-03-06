#include <errno.h>

#include <asterisk.h>
#include <asterisk/astobj2.h>
#include <asterisk/network.h>
#include <asterisk/utils.h>

#include "sccp_debug.h"
#include "sccp_config.h"
#include "sccp_device.h"
#include "sccp_device_registry.h"
#include "sccp_msg.h"
#include "sccp_queue.h"
#include "sccp_session.h"
#include "sccp_task.h"
#include "sccp_utils.h"

static void sccp_session_empty_queue(struct sccp_session *session);

struct sccp_session {
	struct sccp_deserializer deserializer;
	struct sockaddr_in local_addr;
	int sockfd;
	int stop;
	int remote_port;

	struct sccp_cfg *cfg;
	struct sccp_device_registry *registry;
	struct sccp_sync_queue *sync_q;
	struct sccp_task_runner *task_runner;
	struct sccp_device *device;

	char remote_addr_ch[INET_ADDRSTRLEN];
};

enum session_msg_id {
	MSG_NOOP,
	MSG_RELOAD,
};

struct session_msg_reload {
	struct sccp_cfg *cfg;
};

union session_msg_data {
	struct session_msg_reload reload;
};

struct session_msg {
	union session_msg_data data;
	enum session_msg_id id;
};

struct session_task_data_device {
	sccp_device_task_cb callback;
	void *data;
};

union session_task_data {
	struct session_task_data_device device;
};

static void session_msg_init_noop(struct session_msg *msg)
{
	msg->id = MSG_NOOP;
}

static void session_msg_init_reload(struct session_msg *msg, struct sccp_cfg *cfg)
{
	msg->id = MSG_RELOAD;
	msg->data.reload.cfg = cfg;
	ao2_ref(cfg, +1);
}

static void session_msg_destroy(struct session_msg *msg)
{
	switch (msg->id) {
	case MSG_NOOP:
		break;
	case MSG_RELOAD:
		ao2_ref(msg->data.reload.cfg, -1);
		break;
	}
}

/* important to zero out the data memory since sccp_task does a byte level
 * compare to see if two task are equal
 */
static void session_task_zero(union session_task_data *data)
{
	memset(data, 0, sizeof(*data));
}

static void sccp_session_destructor(void *data)
{
	struct sccp_session *session = data;

	if (session->device) {
		/*
		 * This is theoretically impossible, because that would mean:
		 *
		 * - if session->device, then the session is running its session thread
		 * - which would imply that there's still a reference to the session
		 * - which would imply that this function is never called
		 */
		ast_log(LOG_ERROR, "session->device is not null in destructor, something is really wrong\n");
	}

	close(session->sockfd);
	ast_verb(4, "SCCP connection from %s:%d closed\n", session->remote_addr_ch, session->remote_port);

	/* empty the queue here too to handle the case the session was never run */
	sccp_session_empty_queue(session);
	sccp_sync_queue_destroy(session->sync_q);
	sccp_task_runner_destroy(session->task_runner);
	ao2_ref(session->cfg, -1);
}

static int get_sock_local_addr(int sockfd, struct sockaddr_in *addr)
{
	socklen_t slen = sizeof(*addr);

	if (getsockname(sockfd, (struct sockaddr *) addr, &slen)) {
		ast_log(LOG_ERROR, "get session local addr failed: getsockname: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static int set_sock_options(int sockfd)
{
	int flag_nodelay = 1;
	struct timeval flag_timeout = { .tv_sec = 10, .tv_usec = 0 };

	if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag_nodelay, sizeof(flag_nodelay)) == -1) {
		ast_log(LOG_ERROR, "set session sock option failed: setsockopt: %s\n", strerror(errno));
		return -1;
	}

	/*
	 * Setting a send timeout is a must in our case because currently we are doing blocking send.
	 *
	 * This means that, without a timeout, it could stay in send for a long time, which means the
	 * thread session wouldn't read it's alert pipe, and it could take a lot of time before asking
	 * a session to stop and the session thread exiting, which would then create some partial deadlock
	 * condition when closing all sessions, etc.
	 */
	if (setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &flag_timeout, sizeof(flag_timeout)) == -1) {
		ast_log(LOG_ERROR, "set session sock option failed: setsockopt: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

struct sccp_session *sccp_session_create(struct sccp_cfg *cfg, struct sccp_device_registry *registry, struct sockaddr_in *addr, int sockfd)
{
	struct sockaddr_in local_addr;
	struct sccp_sync_queue *sync_q;
	struct sccp_task_runner *task_runner;
	struct sccp_session *session;

	if (!cfg) {
		ast_log(LOG_ERROR, "sccp session create failed: cfg is null\n");
		return NULL;
	}

	if (!registry) {
		ast_log(LOG_ERROR, "sccp session create failed: registry is null\n");
		return NULL;
	}

	if (!addr) {
		ast_log(LOG_ERROR, "sccp session create failed: addr is null\n");
		return NULL;
	}

	if (get_sock_local_addr(sockfd, &local_addr)) {
		return NULL;
	}

	if (set_sock_options(sockfd)) {
		return NULL;
	}

	sync_q = sccp_sync_queue_create(sizeof(struct session_msg));
	if (!sync_q) {
		return NULL;
	}

	task_runner = sccp_task_runner_create(sizeof(union session_task_data));
	if (!task_runner) {
		sccp_sync_queue_destroy(sync_q);
		return NULL;
	}

	session = ao2_alloc(sizeof(*session), sccp_session_destructor);
	if (!session) {
		sccp_task_runner_destroy(task_runner);
		sccp_sync_queue_destroy(sync_q);
		return NULL;
	}

	sccp_deserializer_init(&session->deserializer, sockfd);
	session->local_addr = local_addr;
	session->sockfd = sockfd;
	session->sync_q = sync_q;
	session->task_runner = task_runner;
	session->stop = 0;
	session->device = NULL;
	session->cfg = cfg;
	ao2_ref(cfg, +1);
	session->registry = registry;
	session->remote_port = ntohs(addr->sin_port);
	ast_copy_string(session->remote_addr_ch, ast_inet_ntoa(addr->sin_addr), sizeof(session->remote_addr_ch));

	return session;
}

static void sccp_session_close_queue(struct sccp_session *session)
{
	sccp_sync_queue_close(session->sync_q);
}

static void sccp_session_empty_queue(struct sccp_session *session)
{
	struct sccp_queue q;
	struct session_msg msg;

	sccp_sync_queue_get_all(session->sync_q, &q);
	while (!sccp_queue_get(&q, &msg)) {
		session_msg_destroy(&msg);
	}

	sccp_queue_destroy(&q);
}

static int sccp_session_queue_msg(struct sccp_session *session, struct session_msg *msg)
{
	int ret;

	ret = sccp_sync_queue_put(session->sync_q, msg);
	if (ret) {
		session_msg_destroy(msg);
	}

	return ret;
}

static int sccp_session_queue_msg_noop(struct sccp_session *session)
{
	struct session_msg msg;

	session_msg_init_noop(&msg);

	return sccp_session_queue_msg(session, &msg);
}

static int sccp_session_queue_msg_reload(struct sccp_session *session, struct sccp_cfg *cfg)
{
	struct session_msg msg;

	session_msg_init_reload(&msg, cfg);

	return sccp_session_queue_msg(session, &msg);
}

static void on_auth_timeout(struct sccp_session *session, void __attribute__((unused)) *data)
{
	ast_log(LOG_WARNING, "Device authentication timed out\n");

	session->stop = 1;
}

static int add_auth_timeout_task(struct sccp_session *session)
{
	union session_task_data task_data;

	session_task_zero(&task_data);

	return sccp_task_runner_add(session->task_runner, on_auth_timeout, &task_data, session->cfg->general_cfg->authtimeout);
}

static void remove_auth_timeout_task(struct sccp_session *session)
{
	union session_task_data task_data;

	session_task_zero(&task_data);

	sccp_task_runner_remove(session->task_runner, on_auth_timeout, &task_data);
}

static void process_reload(struct sccp_session *session, struct sccp_cfg *cfg)
{
	struct sccp_device_cfg *device_cfg;

	ao2_ref(session->cfg, -1);
	session->cfg = cfg;
	ao2_ref(cfg, +1);

	if (!session->device) {
		return;
	}

	device_cfg = sccp_cfg_find_device_or_guest(cfg, sccp_device_name(session->device));
	if (!device_cfg) {
		session->stop = 1;
	} else {
		if (sccp_device_reload_config(session->device, device_cfg)) {
			session->stop = 1;
		}

		ao2_ref(device_cfg, -1);
	}
}

static void sccp_session_process_msg(struct sccp_session *session, struct session_msg *msg)
{
	switch (msg->id) {
	case MSG_NOOP:
		break;
	case MSG_RELOAD:
		process_reload(session, msg->data.reload.cfg);
		break;
	}

	session_msg_destroy(msg);
}

static void sccp_session_on_queue_events(struct sccp_session *session, int events)
{
	struct sccp_queue q;
	struct session_msg msg;

	if (events & POLLIN) {
		sccp_sync_queue_get_all(session->sync_q, &q);
		while (!sccp_queue_get(&q, &msg)) {
			sccp_session_process_msg(session, &msg);
		}

		sccp_queue_destroy(&q);
	}

	if (events & ~POLLIN) {
		ast_log(LOG_WARNING, "sccp session on queue events failed: unexpected event 0x%X\n", events);
		session->stop = 1;
	}
}

static int sccp_session_read_sock(struct sccp_session *session)
{
	int ret;

	ret = sccp_deserializer_read(&session->deserializer);
	if (!ret) {
		if (session->device) {
			sccp_device_on_data_read(session->device);
		}

		return 0;
	}

	switch (ret) {
	case SCCP_DESERIALIZER_EOF:
		ast_log(LOG_NOTICE, "Device has closed the connection\n");
		if (session->device) {
			sccp_device_on_connection_lost(session->device);
		}

		break;
	case SCCP_DESERIALIZER_FULL:
		ast_log(LOG_WARNING, "Deserializer buffer is full -- probably invalid or too big message\n");
		break;
	}

	return -1;
}

static int sccp_session_transmit_register_rej(struct sccp_session *session)
{
	struct sccp_msg msg;

	sccp_msg_register_rej(&msg);

	return sccp_session_transmit_msg(session, &msg);
}

static void sccp_session_handle_msg_register(struct sccp_session *session, struct sccp_msg *msg)
{
	struct sccp_device_info device_info;
	struct sccp_device *device;
	struct sccp_device_cfg *device_cfg;
	char *name;
	int ret;

	/* A: session->device is null */

	name = msg->data.reg.name;
	name[sizeof(msg->data.reg.name)] = '\0';

	device_cfg = sccp_cfg_find_device_or_guest(session->cfg, name);
	if (!device_cfg) {
		ast_log(LOG_WARNING, "Device is not configured [%s]\n", name);
		sccp_session_transmit_register_rej(session);
		return;
	}

	device_info.name = name;
	device_info.type = letohl(msg->data.reg.type);
	device_info.proto_version = letohl(msg->data.reg.protoVersion);
	device = sccp_device_create(device_cfg, session, &device_info);
	ao2_ref(device_cfg, -1);
	if (!device) {
		sccp_session_transmit_register_rej(session);
		return;
	}

	ret = sccp_device_registry_add(session->registry, device);
	if (ret) {
		if (ret == SCCP_DEVICE_REGISTRY_ALREADY) {
			ast_log(LOG_WARNING, "Device already registered [%s]\n", name);
		}

		sccp_session_transmit_register_rej(session);
		sccp_device_destroy(device);
		ao2_ref(device, -1);
		return;
	}

	ast_verb(3, "Registered SCCP(%d) '%s' at %s:%d\n", device_info.proto_version, name, session->remote_addr_ch, session->remote_port);

	remove_auth_timeout_task(session);
	sccp_device_on_registration_success(device);

	/* steal the reference ownership */
	session->device = device;
}

static void sccp_session_handle_msg(struct sccp_session *session, struct sccp_msg *msg)
{
	uint32_t msg_id = letohl(msg->id);

	if (sccp_debug_enabled(session->remote_addr_ch)) {
		sccp_dump_message_received(msg, session->remote_addr_ch, session->remote_port);
	}

	if (!session->device) {
		switch (msg_id) {
		case REGISTER_MESSAGE:
			sccp_session_handle_msg_register(session, msg);
			break;
		}
	}

	if (session->device) {
		if (sccp_device_handle_msg(session->device, msg)) {
			session->stop = 1;
		}
	}
}

static void sccp_session_on_sock_events(struct sccp_session *session, int events)
{
	struct sccp_msg *msg;
	int ret;

	if (events & POLLIN) {
		if (sccp_session_read_sock(session)) {
			session->stop = 1;
			return;
		}

		while (!(ret = sccp_deserializer_pop(&session->deserializer, &msg))) {
			sccp_session_handle_msg(session, msg);
		}

		switch (ret) {
		case SCCP_DESERIALIZER_NOMSG:
			break;
		case SCCP_DESERIALIZER_MALFORMED:
			ast_log(LOG_WARNING, "sccp session on sock events failed: malformed message\n");
			session->stop = 1;
			break;
		}
	}

	if (events & ~POLLIN) {
		ast_log(LOG_WARNING, "sccp session on sock events failed: unexpected event 0x%X\n", events);
		session->stop = 1;
	}
}

void sccp_session_run(struct sccp_session *session)
{
	struct pollfd fds[2];
	int nfds;
	int timeout;

	fds[0].fd = session->sockfd;
	fds[0].events = POLLIN;
	fds[1].fd = sccp_sync_queue_fd(session->sync_q);
	fds[1].events = POLLIN;

	add_auth_timeout_task(session);

	for (;;) {
		timeout = sccp_task_runner_next_ms(session->task_runner);

		nfds = poll(fds, ARRAY_LEN(fds), timeout);
		if (nfds == -1) {
			ast_log(LOG_ERROR, "sccp session run failed: poll: %s\n", strerror(errno));
			goto end;
		}

		if (session->stop) {
			goto end;
		}

		if (!nfds) {
			sccp_task_runner_run(session->task_runner, session);
			if (session->stop) {
				goto end;
			}
		} else {
			if (fds[1].revents) {
				sccp_session_on_queue_events(session, fds[1].revents);
				if (session->stop) {
					goto end;
				}
			}

			if (fds[0].revents) {
				sccp_session_on_sock_events(session, fds[0].revents);
				if (session->stop) {
					goto end;
				}
			}
		}
	}

end:
	sccp_session_close_queue(session);
	sccp_session_empty_queue(session);

	if (session->device) {
		/* sccp_device_registry_remove must really be called before
		 * sccp_device_destroy, else undefined behaviour happens, because
		 * registry_remove use some functions that are invalid on destroyed
		 * device
		 */
		sccp_device_registry_remove(session->registry, session->device);
		sccp_device_destroy(session->device);

		ao2_ref(session->device, -1);
		session->device = NULL;
	}
}

int sccp_session_stop(struct sccp_session *session)
{
	int ret;

	/* set session->stop to 1 here so that if called from the session thread,
	 * the flag will be set when going back in the session_run
	 */
	session->stop = 1;

	ret = sccp_session_queue_msg_noop(session);
	if (ret == -1) {
		return -1;
	}

	return 0;
}

int sccp_session_reload_config(struct sccp_session *session, struct sccp_cfg *cfg)
{
	if (!cfg) {
		ast_log(LOG_ERROR, "sccp session reload config failed: cfg is null\n");
		return -1;
	}

	return sccp_session_queue_msg_reload(session, cfg);
}

static void on_device_task_timeout(struct sccp_session *session, void *data)
{
	union session_task_data *task_data = data;

	if (!session->device) {
		ast_log(LOG_ERROR, "on device task timeout failed: session has no device associated\n");
		return;
	}

	task_data->device.callback(session->device, task_data->device.data);
}

int sccp_session_add_device_task(struct sccp_session *session, sccp_device_task_cb callback, void *data, int sec)
{
	union session_task_data task_data;

	session_task_zero(&task_data);
	task_data.device.callback = callback;
	task_data.device.data = data;

	return sccp_task_runner_add(session->task_runner, on_device_task_timeout, &task_data, sec);
}

void sccp_session_remove_device_task(struct sccp_session *session, sccp_device_task_cb callback, void *data)
{
	union session_task_data task_data;

	session_task_zero(&task_data);
	task_data.device.callback = callback;
	task_data.device.data = data;

	sccp_task_runner_remove(session->task_runner, on_device_task_timeout, &task_data);
}

int sccp_session_transmit_msg(struct sccp_session *session, struct sccp_msg *msg)
{
	size_t count = SCCP_MSG_TOTAL_LEN_FROM_LEN(letohl(msg->length));
	ssize_t n;

	if (sccp_debug_enabled(session->remote_addr_ch)) {
		sccp_dump_message_transmitting(msg, session->remote_addr_ch, session->remote_port);
	}

	n = write(session->sockfd, msg, count);
	if (n == (ssize_t) count) {
		return 0;
	}

	session->stop = 1;
	if (n == -1) {
		ast_log(LOG_WARNING, "sccp session transmit msg failed: write: %s\n", strerror(errno));
	} else {
		ast_log(LOG_WARNING, "sccp session transmit msg failed: write wrote less bytes than expected\n");
	}

	return -1;
}

const char *sccp_session_remote_addr_ch(const struct sccp_session *session)
{
	return session->remote_addr_ch;
}

const struct sockaddr_in *sccp_session_local_addr(const struct sccp_session *session)
{
	return &session->local_addr;
}

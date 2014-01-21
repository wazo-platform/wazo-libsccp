#include <errno.h>

#include <asterisk.h>
#include <asterisk/astobj2.h>
#include <asterisk/network.h>
#include <asterisk/utils.h>

#include "sccp_config.h"
#include "sccp_device.h"
#include "sccp_msg.h"
#include "sccp_queue.h"
#include "sccp_session.h"

#define SCCP_MAX_PACKET_SZ 2000

static void sccp_session_empty_queue(struct sccp_session *session);

struct sccp_session {
	struct sccp_deserializer deserializer;
	int sockfd;
	/* XXX don't know if we should really use this or if using return values
	 *     would be better...
	 */
	int quit;

	char outbuf[SCCP_MAX_PACKET_SZ];

	struct sccp_cfg *cfg;
	struct sccp_queue *queue;
	struct sccp_device *device;
};

enum session_msg_id {
	MSG_RELOAD,
	MSG_STOP,
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

static void session_msg_init_reload(struct session_msg *msg, struct sccp_cfg *cfg)
{
	msg->id = MSG_RELOAD;
	msg->data.reload.cfg = cfg;
	ao2_ref(cfg, +1);
}

static void session_msg_init_stop(struct session_msg *msg)
{
	msg->id = MSG_STOP;
}

static void session_msg_destroy(struct session_msg *msg)
{
	switch (msg->id) {
	case MSG_RELOAD:
		ao2_ref(msg->data.reload.cfg, -1);
		break;
	case MSG_STOP:
		break;
	default:
		ast_log(LOG_ERROR, "session msg destroy failed: unknown msg id %d\n", msg->id);
		break;
	}
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

	/* empty the queue here too to handle the case the session was never run */
	sccp_session_empty_queue(session);
	sccp_queue_destroy(session->queue);
	close(session->sockfd);
}

static int set_session_sock_option(int sockfd)
{
	int flag_nodelay;
	struct timeval flag_timeout;

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
	flag_timeout.tv_sec = 10;
	flag_timeout.tv_usec = 0;
	if (setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &flag_timeout, sizeof(flag_timeout)) == -1) {
		ast_log(LOG_ERROR, "set session sock option failed: setsockopt: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

struct sccp_session *sccp_session_create(struct sccp_cfg *cfg, int sockfd)
{
	struct sccp_queue *queue;
	struct sccp_session *session;

	if (!cfg) {
		ast_log(LOG_ERROR, "sccp session create failed: cfg is null\n");
		return NULL;
	}

	if (set_session_sock_option(sockfd)) {
		return NULL;
	}

	queue = sccp_queue_create(sizeof(struct session_msg));
	if (!queue) {
		return NULL;
	}

	session = ao2_alloc(sizeof(*session), sccp_session_destructor);
	if (!session) {
		sccp_queue_destroy(queue);
		return NULL;
	}

	sccp_deserializer_init(&session->deserializer);
	session->sockfd = sockfd;
	session->queue = queue;
	session->quit = 0;
	session->device = NULL;
	session->cfg = cfg;

	return session;
}

static void empty_queue_cb(void *msg_data, void __attribute__((unused)) *arg)
{
	session_msg_destroy((struct session_msg *) msg_data);
}

static void sccp_session_close_queue(struct sccp_session *session)
{
	sccp_queue_close(session->queue);
}

static void sccp_session_empty_queue(struct sccp_session *session)
{
	sccp_queue_process(session->queue, empty_queue_cb, NULL);
}

static int sccp_session_queue_msg(struct sccp_session *session, struct session_msg *msg)
{
	int ret;

	ret = sccp_queue_put(session->queue, msg);
	if (ret) {
		session_msg_destroy(msg);
	}

	return ret;
}

static int sccp_session_queue_msg_reload(struct sccp_session *session, struct sccp_cfg *cfg)
{
	struct session_msg msg;

	session_msg_init_reload(&msg, cfg);

	return sccp_session_queue_msg(session, &msg);
}

static int sccp_session_queue_msg_stop(struct sccp_session *session)
{
	struct session_msg msg;

	session_msg_init_stop(&msg);

	return sccp_session_queue_msg(session, &msg);
}

static void process_queue_cb(void *msg_data, void *arg)
{
	struct session_msg *msg = msg_data;
	struct sccp_session *session = arg;

	switch (msg->id) {
	case MSG_RELOAD:
		ast_debug(1, "received session reload request\n");
		ao2_ref(session->cfg, -1);
		session->cfg = msg->data.reload.cfg;
		ao2_ref(session->cfg, +1);

		if (session->device) {
			sccp_device_reload_config(session->device, msg->data.reload.cfg);
		}
		break;
	case MSG_STOP:
		session->quit = 1;
		break;
	default:
		ast_log(LOG_ERROR, "session process queue: got unknown msg id %d\n", msg->id);
		break;
	}

	session_msg_destroy(msg);
}

void sccp_session_on_queue_events(struct sccp_session *session, int events)
{
	if (events & POLLIN) {
		sccp_queue_process(session->queue, process_queue_cb, session);
	}

	if (events & ~POLLIN) {
		/* unexpected events */
		session->quit = 1;
	}
}

static int sccp_session_read_sock(struct sccp_session *session)
{
	int ret;

	ret = sccp_deserializer_read(&session->deserializer, session->sockfd);
	if (!ret) {
		return 0;
	}

	switch (ret) {
	case SCCP_DESERIALIZER_FULL:
		ast_log(LOG_WARNING, "Deserializer buffer is full -- probably invalid or too big message\n");
		break;
	case SCCP_DESERIALIZER_EOF:
		ast_log(LOG_NOTICE, "Device has closed the connection\n");
		break;
	}

	return -1;
}

void sccp_session_handle_msg(struct sccp_session *session, struct sccp_msg *msg)
{
	/* TODO */
}

void sccp_session_on_sock_events(struct sccp_session *session, int events)
{
	struct sccp_msg *msg;
	int ret;

	if (events & POLLIN) {
		if (sccp_session_read_sock(session)) {
			session->quit = 1;
			return;
		}

		while (!(ret = sccp_deserializer_get(&session->deserializer, &msg))) {
			/* XXX check that session->quit is not none... at some place... */
			sccp_session_handle_msg(session, msg);
		}

		switch (ret) {
		case SCCP_DESERIALIZER_NOMSG:
			break;
		case SCCP_DESERIALIZER_MALFORMED:
			ast_log(LOG_WARNING, "sccp session on sock events failed: malformed message\n");
			session->quit = 1;
			break;
		default:
			ast_log(LOG_WARNING, "sccp session on sock events failed: unknown %d\n", ret);
			session->quit = 1;
			break;
		}
	}

	if (events & ~POLLIN) {
		ast_log(LOG_WARNING, "sccp session on sock events failed: unexpected event\n");
		session->quit = 1;
	}
}

void sccp_session_run(struct sccp_session *session)
{
	struct pollfd fds[2];
	int nfds;

	fds[0].fd = session->sockfd;
	fds[0].events = POLLIN;
	fds[1].fd = sccp_queue_fd(session->queue);
	fds[1].events = POLLIN;

	for (;;) {
		nfds = poll(fds, ARRAY_LEN(fds), -1);
		if (nfds == -1) {
			ast_log(LOG_ERROR, "sccp session run failed: poll: %s\n", strerror(errno));
			goto end;
		}

		if (fds[1].revents) {
			sccp_session_on_queue_events(session, fds[1].revents);
			if (session->quit) {
				goto end;
			}
		}

		if (fds[0].revents) {
			sccp_session_on_sock_events(session, fds[0].revents);
			if (session->quit) {
				goto end;
			}
		}

		/* XXX check device and al. */
		if (session->device) {
			/* FIXME the task running should be done in the device module, not here */
			//lab_device_run_task(session->device);

			if (sccp_device_want_disconnect(session->device)) {
				goto end;
			}

			if (sccp_device_want_unlink(session->device)) {
				/* XXX */
				//sccp_session_dissociate_device(session);
			}
		}
	}

end:
	ast_log(LOG_DEBUG, "leaving session %d thread\n", session->sockfd);

	sccp_session_close_queue(session);
	sccp_session_empty_queue(session);

	if (session->device) {
		/* XXX */
		//sccp_session_dissociate_device(session);
	}
}

int sccp_session_stop(struct sccp_session *session)
{
	return sccp_session_queue_msg_stop(session);
}

int sccp_session_reload_config(struct sccp_session *session, struct sccp_cfg *cfg)
{
	if (!cfg) {
		ast_log(LOG_ERROR, "sccp session reload config failed: cfg is null\n");
		return -1;
	}

	return sccp_session_queue_msg_reload(session, cfg);
}

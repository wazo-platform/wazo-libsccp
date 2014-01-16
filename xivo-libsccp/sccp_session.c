#include <errno.h>

#include <asterisk.h>
#include <asterisk/astobj2.h>
#include <asterisk/network.h>
#include <asterisk/utils.h>

#include "sccp_config.h"
#include "sccp_device.h"
#include "sccp_queue.h"
#include "sccp_session.h"

#define SCCP_MAX_PACKET_SZ 2000

enum session_state {
	STATE_CREATED,
	STATE_RUNNING,
	STATE_ENDED,
};

struct sccp_session {
	/* XXX hum, state is now useless now */
	enum session_state state;
	int sockfd;
	int quit;

	char inbuf[SCCP_MAX_PACKET_SZ];
	char outbuf[SCCP_MAX_PACKET_SZ];

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

	ast_log(LOG_DEBUG, "in destructor for session %p\n", session);

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

	/* XXX should we empty the list of msg in the queue ? */
	sccp_queue_destroy(session->queue);
	close(session->sockfd);
}

static int set_session_socket_option(int sockfd)
{
	int flag_nodelay;
	struct timeval flag_timeout;

	if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag_nodelay, sizeof(flag_nodelay)) == -1) {
		ast_log(LOG_ERROR, "set session socket option failed: setsockopt: %s\n", strerror(errno));
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
		ast_log(LOG_ERROR, "set session socket option failed: setsockopt: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

struct sccp_session *sccp_session_create(int sockfd)
{
	struct sccp_queue *queue;
	struct sccp_session *session;

	if (set_session_socket_option(sockfd)) {
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

	ast_log(LOG_DEBUG, "session %p created\n", session);

	session->state = STATE_CREATED;
	session->sockfd = sockfd;
	session->queue = queue;
	session->quit = 0;
	session->inbuf[0] = '\0';
	session->device = NULL;

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

static void sccp_session_process_queue(struct sccp_session *session)
{
	sccp_queue_process(session->queue, process_queue_cb, session);
}

void sccp_session_run(struct sccp_session *session)
{
	struct pollfd fds[2];
	ssize_t n;
	int nfds;

	session->state = STATE_RUNNING;

	fds[0].fd = session->sockfd;
	fds[0].events = POLLIN;
	fds[1].fd = sccp_queue_fd(session->queue);
	fds[1].events = POLLIN;

	/* XXX maybe we should have 2 "run" function, one for the "session->device" state and one
	 * for the the "session->device is nul" state
	 */
	while (!session->quit) {
		nfds = poll(fds, ARRAY_LEN(fds), -1);
		if (nfds == -1) {
			ast_log(LOG_ERROR, "sccp session run failed: poll: %s\n", strerror(errno));
			goto end;
		}

		if (fds[1].revents & POLLIN) {
			sccp_session_process_queue(session);
			if (session->quit) {
				goto end;
			}
		} else if (fds[1].revents) {
			/* unexpected events */
			goto end;
		}

		if (fds[0].revents & POLLIN) {
			n = read(session->sockfd, session->inbuf, sizeof(session->inbuf) - 1);
			if (n <= 0) {
				goto end;
			}

			session->inbuf[n-1] = '\0';
			/* FIXME handle the input */
			//sccp_session_handle_input(session);
		} else if (fds[0].revents) {
			/* unexpected events */
			goto end;
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

	session->state = STATE_ENDED;

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

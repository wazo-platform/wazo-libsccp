#include <fcntl.h>
#include <unistd.h>

#include <asterisk.h>
#include <asterisk/astobj2.h>
#include <asterisk/linkedlists.h>
#include <asterisk/network.h>
#include <asterisk/utils.h>

#include "sccp_device.h"
#include "sccp_session.h"

#define SCCP_MAX_PACKET_SZ 2000

struct sccp_session {
	int sockfd;
	int pipefd[2];
	int running;
	int request_stop;
	int quit;

	AST_LIST_HEAD_NOLOCK(, session_msg) msgs;

	char inbuf[SCCP_MAX_PACKET_SZ];
	char outbuf[SCCP_MAX_PACKET_SZ];

	struct sccp_device *device;
};

enum session_msg_id {
	MSG_RELOAD,
	MSG_STOP,
};

struct session_msg {
	enum session_msg_id id;

	AST_LIST_ENTRY(session_msg) list;
};

static struct session_msg *session_msg_create(enum session_msg_id id)
{
	struct session_msg *msg;

	msg = ast_calloc(1, sizeof(*msg));
	if (!msg) {
		return NULL;
	}

	msg->id = id;

	return msg;
}

static void session_msg_free(struct session_msg *msg)
{
	ast_free(msg);
}

static void sccp_session_destructor(void *data)
{
	struct sccp_session *session = data;
	struct session_msg *msg;

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

	/* XXX normally, the list should already be empty... */
	AST_LIST_TRAVERSE_SAFE_BEGIN(&session->msgs, msg, list) {
		AST_LIST_REMOVE_CURRENT(list);
		session_msg_free(msg);
	}
	AST_LIST_TRAVERSE_SAFE_END;

	close(session->sockfd);
	close(session->pipefd[0]);
	close(session->pipefd[1]);
}

struct sccp_session *sccp_session_create(int sockfd)
{
	struct sccp_session *session;
	int pipefd[2];
	int flag_nodelay;
	struct timeval flag_timeout;

	if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag_nodelay, sizeof(flag_nodelay)) == -1) {
		ast_log(LOG_ERROR, "sccp session create failed: setsockopt: %s\n", strerror(errno));
		return NULL;
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
		ast_log(LOG_ERROR, "sccp session create failed: setsockopt: %s\n", strerror(errno));
		return NULL;
	}

	if (pipe2(pipefd, O_NONBLOCK)) {
		ast_log(LOG_ERROR, "sccp session create failed: pipe: %s\n", strerror(errno));
		return NULL;
	}

	session = ao2_alloc(sizeof(*session), sccp_session_destructor);
	if (!session) {
		close(pipefd[0]);
		close(pipefd[1]);
		return NULL;
	}

	ast_log(LOG_DEBUG, "session %p created\n", session);

	session->sockfd = sockfd;
	session->pipefd[0] = pipefd[0];
	session->pipefd[1] = pipefd[1];
	session->running = 0;
	session->request_stop = 0;
	session->quit = 0;
	AST_LIST_HEAD_INIT_NOLOCK(&session->msgs);
	session->inbuf[0] = '\0';
	session->device = NULL;

	return session;
}

static int sccp_session_write_to_pipe(struct sccp_session *session)
{
	static const int pipeval = 0xF00BA7;
	ssize_t n;

	n = write(session->pipefd[1], &pipeval, sizeof(pipeval));

	switch (n) {
	case -1:
		ast_log(LOG_ERROR, "session write to pipe failed: write: %s\n", strerror(errno));
		return -1;
	case 0:
		ast_log(LOG_ERROR, "session write to pipe failed: write wrote nothing\n");
		return -1;
	}

	return 0;
}

static int sccp_session_read_from_pipe(struct sccp_session *session)
{
	char buf[32];
	ssize_t n;

	n = read(session->pipefd[0], buf, sizeof(buf));

	switch (n) {
	case -1:
		ast_log(LOG_ERROR, "session read pipe failed: read: %s\n", strerror(errno));
		return -1;
	case 0:
		ast_log(LOG_ERROR, "session read to pipe failed: end of file reached\n");
		return -1;
	}

	return 0;
}

/*
 * The session lock must be acquired before calling this function.
 */
static int sccp_session_queue_msg(struct sccp_session *session, enum session_msg_id msg_id)
{
	struct session_msg *msg;

	if (!session->running) {
		ast_log(LOG_NOTICE, "session queue msg failed: session not running\n");
		return -1;
	}

	if (session->request_stop) {
		/* don't queue more msg if a stop has already been requested */
		ast_log(LOG_NOTICE, "session queue msg failed: session is stopping\n");
		return -1;
	}

	msg = session_msg_create(msg_id);
	if (!msg) {
		return -1;
	}

	if (msg_id == MSG_STOP) {
		session->request_stop = 1;
	}

	AST_LIST_INSERT_TAIL(&session->msgs, msg, list);
	sccp_session_write_to_pipe(session);

	return 0;
}

void sccp_session_run(struct sccp_session *session)
{
	struct session_msg *msg, *nextmsg;
	struct pollfd fds[2];
	ssize_t n;
	int nfds;
	int stop = 0;

	/* XXX hum, session->running was previously set just before calling
	 *     ast_pthread_create. now, setting it in the running thread
	 *     means that a session might be running even if the session->
	 *     running is set to 0, which is probably bad
	 *     --> in fact, we probably don't need to check if running before
	 *         asking to stop, it will stop eventually
	 */
	session->running = 1;
	session->request_stop = 0;

	fds[0].fd = session->sockfd;
	fds[0].events = POLLIN;
	fds[1].fd = session->pipefd[0];
	fds[1].events = POLLIN;

	snprintf(session->outbuf, sizeof(session->outbuf), "sockfd: %d\n\n", session->sockfd);
	n = write(session->sockfd, session->outbuf, strlen(session->outbuf));
	if (n <= 0) {
		ast_log(LOG_WARNING, "sccp session run failed: write: %s\n", strerror(errno));
		goto end;
	}

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
			ao2_lock(session);
			nextmsg = AST_LIST_FIRST(&session->msgs);
			AST_LIST_HEAD_INIT_NOLOCK(&session->msgs);
			sccp_session_read_from_pipe(session);
			ao2_unlock(session);

			while ((msg = nextmsg)) {
				nextmsg = AST_LIST_NEXT(msg, list);

				switch (msg->id) {
				case MSG_RELOAD:
					if (session->device) {
						sccp_device_reload_config(session->device);
					}
					break;
				case MSG_STOP:
					/* don't goto end now, first process all message so we don't leak */
					stop = 1;
					break;
				}

				session_msg_free(msg);
			}

			if (stop) {
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

	ao2_lock(session);
	session->running = 0;
	ao2_unlock(session);

	/* XXX we probably want to process the msgs here too */

	if (session->device) {
		/* XXX */
		//sccp_session_dissociate_device(session);
	}
}

int sccp_session_stop(struct sccp_session *session)
{
	int ret;

	ao2_lock(session);
	ret = sccp_session_queue_msg(session, MSG_STOP);
	ao2_unlock(session);

	return ret;
}

int sccp_session_reload_config(struct sccp_session *session, struct sccp_cfg *cfg)
{
	int ret;

	/* TODO do something with the cfg */

	ao2_lock(session);
	ret = sccp_session_queue_msg(session, MSG_RELOAD);
	ao2_unlock(session);

	return ret;
}

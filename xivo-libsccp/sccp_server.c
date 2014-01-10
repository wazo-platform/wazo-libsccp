#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <asterisk.h>
#include <asterisk/cli.h>
#include <asterisk/linkedlists.h>
#include <asterisk/lock.h>
#include <asterisk/network.h>

#include "sccp_config.h"
#include "sccp_server.h"
#include "sccp_session.h"

#define SERVER_PORT 2000
#define SERVER_BACKLOG 50

static void *server_loop(void *data);

struct server {
	int sockfd;
	int pipefd[2];
	int running;
	int request_stop;
	int session_count;

	AST_LIST_HEAD_NOLOCK(, server_msg) msgs;

	pthread_t thread;
	ast_mutex_t lock;
	ast_cond_t no_session_cond;

	struct ao2_container *sessions;
};

enum server_msg_id {
	MSG_RELOAD,
	MSG_STOP,
};

struct server_msg {
	enum server_msg_id id;

	AST_LIST_ENTRY(server_msg) list;
};

static struct server global_server;

static struct server_msg *server_msg_create(enum server_msg_id id)
{
	struct server_msg *msg;

	msg = ast_calloc(1, sizeof(*msg));
	if (!msg) {
		return NULL;
	}

	msg->id = id;

	return msg;
}

static void server_msg_free(struct server_msg *msg)
{
	ast_free(msg);
}

static int server_init(struct server *server)
{
	struct ao2_container *sessions;

	/* XXX hum, what's the best container parameter for sessions ? */
	sessions = ao2_container_alloc(1, NULL, NULL);
	if (!sessions) {
		return -1;
	}

	if (pipe2(server->pipefd, O_NONBLOCK) == -1) {
		ast_log(LOG_ERROR, "server init: pipe: %s\n", strerror(errno));
		ao2_ref(sessions, -1);
		return -1;
	}

	server->running = 0;
	server->request_stop = 0;
	AST_LIST_HEAD_INIT_NOLOCK(&server->msgs);
	ast_mutex_init(&server->lock);
	ast_cond_init(&server->no_session_cond, NULL);
	server->sessions = sessions;

	return 0;
}

/*
 * The server thread must not be running, i.e. if the server has been started
 * successfully, it must have been stopped and joined successfully too.
 */
static void server_destroy(struct server *server)
{
	struct server_msg *msg;

	/* XXX normally, the list should already be empty... */
	AST_LIST_TRAVERSE_SAFE_BEGIN(&server->msgs, msg, list) {
		AST_LIST_REMOVE_CURRENT(list);
		server_msg_free(msg);
	}
	AST_LIST_TRAVERSE_SAFE_END;

	ast_mutex_destroy(&server->lock);
	ast_cond_destroy(&server->no_session_cond);
	close(server->pipefd[0]);
	close(server->pipefd[1]);

	ao2_ref(server->sessions, -1);
}

static int server_write_to_pipe(struct server *server)
{
	static const int pipeval = 0xF00BA7;
	ssize_t n;

	n = write(server->pipefd[1], &pipeval, sizeof(pipeval));

	switch (n) {
	case -1:
		ast_log(LOG_ERROR, "server write to pipe failed: write: %s\n", strerror(errno));
		return -1;
	case 0:
		ast_log(LOG_ERROR, "server write to pipe failed: write wrote nothing\n");
		return -1;
	}

	return 0;
}

static int server_read_from_pipe(struct server *server)
{
	char buf[32];
	ssize_t n;

	n = read(server->pipefd[0], buf, sizeof(buf));

	switch (n) {
	case -1:
		ast_log(LOG_ERROR, "server read pipe failed: read: %s\n", strerror(errno));
		return -1;
	case 0:
		ast_log(LOG_ERROR, "server read to pipe failed: end of file reached\n");
		return -1;
	}

	return 0;
}

static int server_start(struct server *server)
{
	struct sockaddr_in addr;
	int flag_reuse = 1;
	int ret;

	if (server->running) {
		ast_log(LOG_ERROR, "server start failed: server already running\n");
		return -1;
	}

	/* FIXME take into account bindaddr from the general config */

	server->sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (server->sockfd == -1) {
		ast_log(LOG_ERROR, "server start failed: socket: %s\n", strerror(errno));
		goto error;
	}

	if (setsockopt(server->sockfd, SOL_SOCKET, SO_REUSEADDR, &flag_reuse, sizeof(flag_reuse)) == -1) {
		ast_log(LOG_ERROR, "server start failed: setsockopt: %s\n", strerror(errno));
		goto error;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(SERVER_PORT);
	addr.sin_addr.s_addr = INADDR_ANY;
	if (bind(server->sockfd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
		ast_log(LOG_ERROR, "server start failed: bind: %s\n", strerror(errno));
		goto error;
	}

	server->running = 1;
	server->request_stop = 0;
	ret = ast_pthread_create_background(&server->thread, NULL, server_loop, server);
	if (ret) {
		ast_log(LOG_ERROR, "server start failed: pthread create: %s\n", strerror(ret));
		goto error;
	}

	return 0;

error:
	if (server->sockfd != -1) {
		close(server->sockfd);
	}
	server->running = 0;

	return -1;
}

/*
 * The server lock must be acquired before calling this function.
 */
static int server_queue_msg(struct server *server, enum server_msg_id msg_id)
{
	struct server_msg *msg;

	if (!server->running) {
		ast_log(LOG_NOTICE, "server queue msg failed: server not running\n");
		return -1;
	}

	if (server->request_stop) {
		/* don't queue more msg if a stop has already been requested */
		ast_log(LOG_NOTICE, "server queue msg failed: server is stopping\n");
		return -1;
	}

	msg = server_msg_create(msg_id);
	if (!msg) {
		return -1;
	}

	if (msg_id == MSG_STOP) {
		server->request_stop = 1;
	}

	AST_LIST_INSERT_TAIL(&server->msgs, msg, list);
	server_write_to_pipe(server);

	return 0;
}

static int server_reload_config(struct server *server)
{
	int ret;

	ast_mutex_lock(&server->lock);
	ret = server_queue_msg(server, MSG_RELOAD);
	ast_mutex_unlock(&server->lock);

	return ret;
}

static int server_stop(struct server *server)
{
	int ret;

	ast_mutex_lock(&server->lock);
	ret = server_queue_msg(server, MSG_STOP);
	ast_mutex_unlock(&server->lock);

	return ret;
}

/*
 * Can be called a maximum once and only if server_stop has been called
 * successfully before.
 */
static int server_join(struct server *server)
{
	int ret;

	if (!server->request_stop) {
		ast_log(LOG_ERROR, "server join failed: not asked to stop\n");
		return -1;
	}

	ast_log(LOG_DEBUG, "waiting for server thread to exit\n");
	ret = pthread_join(server->thread, NULL);
	if (ret) {
		ast_log(LOG_ERROR, "server join failed: pthread_join: %s\n", strerror(ret));
		return -1;
	}

	return 0;
}


static int cb_stop_session(void *obj, void *arg, int flags)
{
	struct sccp_session *session = obj;

	sccp_session_stop(session);

	return 0;
}

static void server_stop_sessions(struct server *server)
{
	ao2_callback(server->sessions, OBJ_NODATA, cb_stop_session, NULL);
}

static int cb_reload_session(void *obj, void *arg, int flags)
{
	struct sccp_session *session = obj;

	sccp_session_reload_config(session);

	return 0;
}

static void server_reload_sessions(struct server *server)
{
	ao2_callback(server->sessions, OBJ_NODATA, cb_reload_session, NULL);
}

static int server_add_session(struct server *server, struct sccp_session *session)
{
	if (!ao2_link(server->sessions, session)) {
		return -1;
	}

	ast_mutex_lock(&server->lock);
	server->session_count++;
	ast_mutex_unlock(&server->lock);

	return 0;
}

static void server_remove_session(struct server *server, struct sccp_session *session)
{
	ao2_unlink(server->sessions, session);

	ast_mutex_lock(&server->lock);
	server->session_count--;
	if (!server->session_count) {
		ast_cond_signal(&server->no_session_cond);
	}
	ast_mutex_unlock(&server->lock);
}

static void *session_run(void *data)
{
	struct sccp_session *session = data;

	sccp_session_run(session);

	/* using the global variable, because meh, simpler */
	server_remove_session(&global_server, session);

	ao2_ref(session, -1);

	return NULL;
}

static int server_start_session(struct server *server, struct sccp_session *session)
{
	pthread_t thread;
	int ret;

	ret = ast_pthread_create_detached(&thread, NULL, session_run, session);
	if (ret) {
		ast_log(LOG_ERROR, "server start session failed: pthread create: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static void server_wait_sessions(struct server *server)
{
	ast_log(LOG_DEBUG, "waiting for all sessions to exit\n");

	ast_mutex_lock(&server->lock);
	while (server->session_count) {
		ast_cond_wait(&server->no_session_cond, &server->lock);
	}
	ast_mutex_unlock(&server->lock);
}

static void *server_loop(void *data)
{
	struct server *server = data;
	struct server_msg *msg;
	struct server_msg *nextmsg;
	struct sccp_session *session;
	struct pollfd fds[2];
	int nfds;
	int sockfd;
	int stop = 0;
	struct sockaddr_in addr;
	socklen_t addrlen;

	if (listen(server->sockfd, SERVER_BACKLOG) == -1) {
		ast_log(LOG_ERROR, "server loop failed: listen: %s\n", strerror(errno));
		goto end;
	}

	fds[0].fd = server->sockfd;
	fds[0].events = POLLIN;
	fds[1].fd = server->pipefd[0];
	fds[1].events = POLLIN;

	for (;;) {
		nfds = poll(fds, ARRAY_LEN(fds), -1);
		if (nfds == -1) {
			ast_log(LOG_ERROR, "server loop failed: poll: %s\n", strerror(errno));
			goto end;
		}

		if (fds[1].revents & POLLIN) {
			ast_mutex_lock(&server->lock);
			nextmsg = AST_LIST_FIRST(&server->msgs);
			AST_LIST_HEAD_INIT_NOLOCK(&server->msgs);
			server_read_from_pipe(server);
			ast_mutex_unlock(&server->lock);

			while ((msg = nextmsg)) {
				nextmsg = AST_LIST_NEXT(msg, list);

				switch (msg->id) {
				case MSG_RELOAD:
					server_reload_sessions(server);
					break;
				case MSG_STOP:
					/* don't goto end now, first process all message so we don't leak */
					stop = 1;
					break;
				}

				server_msg_free(msg);
			}

			if (stop) {
				goto end;
			}
		} else if (fds[1].revents) {
			/* unexpected events */
			goto end;
		}

		if (fds[0].revents & POLLIN) {
			addrlen = sizeof(addr);
			sockfd = accept(server->sockfd, (struct sockaddr *)&addr, &addrlen);
			if (sockfd == -1) {
				ast_log(LOG_ERROR, "server loop failed: accept: %s\n", strerror(errno));
				goto end;
			}

			ast_verb(4, "New connection from %s:%d accepted\n", ast_inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

			session = sccp_session_create(sockfd);
			if (!session) {
				close(sockfd);
				goto end;
			}

			if (server_add_session(server, session)) {
				ao2_ref(session, -1);
				goto end;
			}

			if (server_start_session(server, session)) {
				server_remove_session(server, session);
				ao2_ref(session, -1);
				goto end;
			}
		} else if (fds[0].revents) {
			/* unexpected events */
			goto end;
		}
	}

end:
	ast_log(LOG_DEBUG, "leaving server thread\n");

	close(server->sockfd);

	ast_mutex_lock(&server->lock);
	server->running = 0;
	ast_mutex_unlock(&server->lock);

	/* XXX we probably want to process the msgs here too */

	return NULL;
}

static char *cli_show_sessions(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "sccp show sessions";
		e->usage = "Usage: sccp show sessions\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	ast_cli(a->fd, "%d active sessions\n", global_server.session_count);

	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_entries[] = {
	AST_CLI_DEFINE(cli_show_sessions, "Show the active sessions"),
};

int sccp_server_init(void)
{
	if (server_init(&global_server)) {
		return -1;
	}

	ast_cli_register_multiple(cli_entries, ARRAY_LEN(cli_entries));

	return 0;
}

void sccp_server_destroy(void)
{
	ast_cli_unregister_multiple(cli_entries, ARRAY_LEN(cli_entries));

	if (!server_stop(&global_server)) {
		server_join(&global_server);
	}

	server_stop_sessions(&global_server);
	server_wait_sessions(&global_server);
	server_destroy(&global_server);
}

int sccp_server_start(void)
{
	return server_start(&global_server);
}

int sccp_server_reload_config(void)
{
	return server_reload_config(&global_server);
}

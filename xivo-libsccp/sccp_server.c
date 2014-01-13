#include <errno.h>

#include <asterisk.h>
#include <asterisk/cli.h>
#include <asterisk/lock.h>
#include <asterisk/network.h>

#include "sccp_config.h"
#include "sccp_queue.h"
#include "sccp_server.h"
#include "sccp_session.h"

#define SERVER_PORT 2000
#define SERVER_BACKLOG 50

static void *server_run(void *data);

struct server {
	int sockfd;
	int running;
	int request_stop;
	int session_count;
	int quit;

	pthread_t thread;
	ast_mutex_t lock;
	ast_cond_t no_session_cond;

	struct sccp_queue *queue;
	struct ao2_container *sessions;
};

enum server_msg_id {
	MSG_RELOAD,
	MSG_STOP,
};

struct server_msg_reload {
	struct sccp_cfg *cfg;
};

union server_msg_data {
	struct server_msg_reload reload;
};

struct server_msg {
	union server_msg_data data;
	enum server_msg_id id;
};

static struct server global_server;

static void server_msg_init_reload(struct server_msg *msg, struct sccp_cfg *cfg)
{
	msg->id = MSG_RELOAD;
	msg->data.reload.cfg = cfg;
	ao2_ref(cfg, +1);
}

static void server_msg_init_stop(struct server_msg *msg)
{
	msg->id = MSG_STOP;
}

static void server_msg_free(struct server_msg *msg)
{
	switch (msg->id) {
	case MSG_RELOAD:
		ao2_ref(msg->data.reload.cfg, -1);
		break;
	default:
		break;
	}
}

static int server_init(struct server *server)
{
	server->queue = sccp_queue_create(sizeof(struct server_msg));
	if (!server->queue) {
		return -1;
	}

	/* XXX hum, what's the best container parameter for sessions ?
	 *     in fact, we should probably use a linked list of
	 *     session if we only need to link / unlink and apply callback to
	 *     each
	 */
	server->sessions = ao2_container_alloc(1, NULL, NULL);
	if (!server->sessions) {
		sccp_queue_destroy(server->queue);
		return -1;
	}

	server->running = 0;
	server->request_stop = 0;
	ast_mutex_init(&server->lock);
	ast_cond_init(&server->no_session_cond, NULL);

	return 0;
}

/*
 * The server thread must not be running, i.e. if the server has been started
 * successfully, it must have been stopped and joined successfully too.
 */
static void server_destroy(struct server *server)
{
	ast_mutex_destroy(&server->lock);
	ast_cond_destroy(&server->no_session_cond);
	sccp_queue_destroy(server->queue);
	ao2_ref(server->sessions, -1);
}

/*
 * The server lock must be acquired before calling this function.
 */
static int server_queue_msg(struct server *server, struct server_msg *msg)
{
	if (!server->running) {
		ast_log(LOG_NOTICE, "server queue msg failed: server not running\n");
		return -1;
	}

	if (server->request_stop) {
		/* don't queue more msg if a stop has already been requested */
		ast_log(LOG_NOTICE, "server queue msg failed: server is stopping\n");
		return -1;
	}

	if (sccp_queue_put(server->queue, msg)) {
		return -1;
	}

	if (msg->id == MSG_STOP) {
		server->request_stop = 1;
	}

	return 0;
}

static int server_reload_config(struct server *server, struct sccp_cfg *cfg)
{
	struct server_msg msg;
	int ret;

	server_msg_init_reload(&msg, cfg);

	ast_mutex_lock(&server->lock);
	ret = server_queue_msg(server, &msg);
	ast_mutex_unlock(&server->lock);

	return ret;
}

static int server_stop(struct server *server)
{
	struct server_msg msg;
	int ret;

	server_msg_init_stop(&msg);

	ast_mutex_lock(&server->lock);
	ret = server_queue_msg(server, &msg);
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
	ast_log(LOG_DEBUG, "processing server stop request\n");

	ao2_callback(server->sessions, OBJ_NODATA, cb_stop_session, NULL);
}

static int cb_reload_session(void *obj, void *arg, int flags)
{
	struct sccp_session *session = obj;
	struct sccp_cfg *cfg = arg;

	sccp_session_reload_config(session, cfg);

	return 0;
}

static void server_reload_sessions(struct server *server, struct sccp_cfg *cfg)
{
	ast_log(LOG_DEBUG, "processing server reload request\n");

	ao2_callback(server->sessions, OBJ_NODATA, cb_reload_session, cfg);
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

static void server_process_queue(struct server *server)
{
	struct server_msg msg;

	for (;;) {
		/* XXX this is not terribly efficient, but since we currently have
		 * a very low volume of msg in the queue, guess that's just fine
		 *
		 * else, we could read X message at a time between a lock/unlock
		 * store them on a linked list, ...
		 */
		ast_mutex_lock(&server->lock);
		if (sccp_queue_empty(server->queue)) {
			ast_mutex_unlock(&server->lock);
			return;
		}

		sccp_queue_get(server->queue, &msg);
		ast_mutex_unlock(&server->lock);

		switch (msg.id) {
		case MSG_RELOAD:
			server_reload_sessions(server, msg.data.reload.cfg);
			break;
		case MSG_STOP:
			server->quit = 1;
			break;
		default:
			ast_log(LOG_ERROR, "server process queue: got unknown msg id %d\n", msg.id);
		}

		server_msg_free(&msg);
	}
}

static int new_server_socket(struct sccp_cfg *cfg)
{
	struct sockaddr_in addr;
	int sockfd;
	int flag_reuse = 1;

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sockfd == -1) {
		ast_log(LOG_ERROR, "server new socket failed: %s\n", strerror(errno));
		return -1;
	}

	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &flag_reuse, sizeof(flag_reuse)) == -1) {
		ast_log(LOG_ERROR, "server new socket failed: setsockopt: %s\n", strerror(errno));
		close(sockfd);
		return -1;
	}

	/* FIXME take into account bindaddr from the config */
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(SERVER_PORT);
	addr.sin_addr.s_addr = INADDR_ANY;
	if (bind(sockfd, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
		ast_log(LOG_ERROR, "server new socket failed: bind: %s\n", strerror(errno));
		close(sockfd);
		return -1;
	}

	return sockfd;
}

static int server_start(struct server *server, struct sccp_cfg *cfg)
{
	int ret;

	if (server->running) {
		ast_log(LOG_ERROR, "server start failed: server already running\n");
		return -1;
	}

	server->sockfd = new_server_socket(cfg);
	if (server->sockfd == -1) {
		return -1;
	}

	server->running = 1;
	server->request_stop = 0;
	ret = ast_pthread_create_background(&server->thread, NULL, server_run, server);
	if (ret) {
		ast_log(LOG_ERROR, "server start failed: pthread create: %s\n", strerror(ret));
		close(server->sockfd);
		server->running = 0;
	}

	return 0;
}

static void *server_run(void *data)
{
	struct server *server = data;
	struct sccp_session *session;
	struct pollfd fds[2];
	struct sockaddr_in addr;
	socklen_t addrlen;
	int nfds;
	int sockfd;

	if (listen(server->sockfd, SERVER_BACKLOG) == -1) {
		ast_log(LOG_ERROR, "server run failed: listen: %s\n", strerror(errno));
		goto end;
	}

	fds[0].fd = server->sockfd;
	fds[0].events = POLLIN;
	fds[1].fd = sccp_queue_fd(server->queue);
	fds[1].events = POLLIN;

	server->quit = 0;
	while (!server->quit) {
		nfds = poll(fds, ARRAY_LEN(fds), -1);
		if (nfds == -1) {
			ast_log(LOG_ERROR, "server run failed: poll: %s\n", strerror(errno));
			goto end;
		}

		if (fds[1].revents & POLLIN) {
			server_process_queue(server);
			if (server->quit) {
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
				ast_log(LOG_ERROR, "server run failed: accept: %s\n", strerror(errno));
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

int sccp_server_start(struct sccp_cfg *cfg)
{
	if (!cfg) {
		ast_log(LOG_ERROR, "sccp server start failed: cfg is null\n");
		return -1;
	}

	return server_start(&global_server, cfg);
}

int sccp_server_reload_config(struct sccp_cfg *cfg)
{
	if (!cfg) {
		ast_log(LOG_ERROR, "sccp server reload config failed: cfg is null\n");
		return -1;
	}

	return server_reload_config(&global_server, cfg);
}

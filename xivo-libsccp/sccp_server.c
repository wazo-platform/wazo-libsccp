#include <errno.h>

#include <asterisk.h>
#include <asterisk/cli.h>
#include <asterisk/linkedlists.h>
#include <asterisk/lock.h>
#include <asterisk/network.h>

#include "sccp_config.h"
#include "sccp_queue.h"
#include "sccp_server.h"
#include "sccp_session.h"

#define SERVER_PORT 2000
#define SERVER_BACKLOG 50

static void *server_run(void *data);

enum server_state {
	STATE_UNINITIALIZED,
	STATE_INITIALIZED,
	STATE_RUNNING,
	STATE_ENDED,
};

struct server {
	enum server_state state;
	int sockfd;
	int quit;

	pthread_t thread;
	ast_mutex_t lock;

	struct sccp_queue *queue;
	/*
	 * When the server is in running state, only the server thread modify the
	 * srv_sessions list. That said, lock are needed on add / remove since another
	 * thread could be running a "sccp show session"...
	 */
	AST_LIST_HEAD(, server_session) srv_sessions;
};

struct server_session {
	AST_LIST_ENTRY(server_session) list;
	struct sccp_session *session;
	pthread_t thread;
};

enum server_msg_id {
	MSG_RELOAD,
	MSG_SESSION_END,
	MSG_STOP,
};

struct server_msg_reload {
	struct sccp_cfg *cfg;
};

struct server_msg_session_end {
	struct server_session *srv_session;
};

union server_msg_data {
	struct server_msg_reload reload;
	struct server_msg_session_end session_end;
};

struct server_msg {
	union server_msg_data data;
	enum server_msg_id id;
};

static struct server global_server = {
	.state = STATE_UNINITIALIZED,
};

/*
 * On success, the function take ownership of the session (i.e. steal the reference)
 */
static struct server_session *server_session_create(struct sccp_session *session)
{
	struct server_session *srv_session;

	srv_session = ast_calloc(1, sizeof(*srv_session));
	if (!srv_session) {
		return NULL;
	}

	srv_session->session = session;

	return srv_session;
}

static void server_session_destroy(struct server_session *srv_session)
{
	ao2_ref(srv_session->session, -1);
	ast_free(srv_session);
}

static void server_msg_init_reload(struct server_msg *msg, struct sccp_cfg *cfg)
{
	msg->id = MSG_RELOAD;
	msg->data.reload.cfg = cfg;
	ao2_ref(cfg, +1);
}

static void server_msg_init_session_end(struct server_msg *msg, struct server_session *srv_session)
{
	msg->id = MSG_SESSION_END;
	msg->data.session_end.srv_session = srv_session;
}

static void server_msg_init_stop(struct server_msg *msg)
{
	msg->id = MSG_STOP;
}

static void server_msg_destroy(struct server_msg *msg)
{
	switch (msg->id) {
	case MSG_RELOAD:
		ao2_ref(msg->data.reload.cfg, -1);
		break;
	case MSG_SESSION_END:
	case MSG_STOP:
		break;
	default:
		ast_log(LOG_ERROR, "server msg destroy failed: unknown msg id %d\n", msg->id);
		break;
	}
}

static int server_init(struct server *server)
{
	server->queue = sccp_queue_create(sizeof(struct server_msg));
	if (!server->queue) {
		return -1;
	}

	server->state = STATE_INITIALIZED;
	ast_mutex_init(&server->lock);
	AST_LIST_HEAD_INIT(&server->srv_sessions);

	return 0;
}

static void server_destroy(struct server *server)
{
	sccp_queue_destroy(server->queue);
	ast_mutex_destroy(&server->lock);
	AST_LIST_HEAD_DESTROY(&server->srv_sessions);
	server->state = STATE_UNINITIALIZED;
}


static void server_empty_queue(struct server *server)
{
	struct server_msg msg;

	while (!sccp_queue_is_empty(server->queue)) {
		sccp_queue_get(server->queue, &msg);
		server_msg_destroy(&msg);
	}
}

static int server_queue_msg_no_lock(struct server *server, struct server_msg *msg)
{
	if (server->state != STATE_RUNNING) {
		return -1;
	}

	if (sccp_queue_put(server->queue, msg)) {
		return -1;
	}

	return 0;
}

static int server_queue_msg(struct server *server, struct server_msg *msg)
{
	int ret;

	ast_mutex_lock(&server->lock);
	ret = server_queue_msg_no_lock(server, msg);
	ast_mutex_unlock(&server->lock);

	if (ret) {
		server_msg_destroy(msg);
	}

	return ret;
}

static int server_queue_msg_reload(struct server *server, struct sccp_cfg *cfg)
{
	struct server_msg msg;

	server_msg_init_reload(&msg, cfg);

	return server_queue_msg(server, &msg);
}

static int server_queue_msg_session_end(struct server *server, struct server_session *srv_session)
{
	struct server_msg msg;

	server_msg_init_session_end(&msg, srv_session);

	return server_queue_msg(server, &msg);
}

static int server_queue_msg_stop(struct server *server)
{
	struct server_msg msg;

	server_msg_init_stop(&msg);

	return server_queue_msg(server, &msg);
}

static int server_join(struct server *server)
{
	int ret;

	ast_debug(1, "joining server thread\n");
	ret = pthread_join(server->thread, NULL);
	if (ret) {
		ast_log(LOG_ERROR, "server join failed: pthread_join: %s\n", strerror(ret));
		return -1;
	}

	return 0;
}

static void server_stop_sessions(struct server *server)
{
	struct server_session *srv_session;

	AST_LIST_TRAVERSE(&server->srv_sessions, srv_session, list) {
		sccp_session_stop(srv_session->session);
	}
}

static void server_reload_sessions(struct server *server, struct sccp_cfg *cfg)
{
	struct server_session *srv_session;

	AST_LIST_TRAVERSE(&server->srv_sessions, srv_session, list) {
		sccp_session_reload_config(srv_session->session, cfg);
	}
}

static void server_add_srv_session(struct server *server, struct server_session *srv_session)
{
	AST_LIST_LOCK(&server->srv_sessions);
	AST_LIST_INSERT_TAIL(&server->srv_sessions, srv_session, list);
	AST_LIST_UNLOCK(&server->srv_sessions);
}

static void server_remove_srv_session(struct server *server, struct server_session *srv_session)
{
	AST_LIST_LOCK(&server->srv_sessions);
	AST_LIST_REMOVE(&server->srv_sessions, srv_session, list);
	AST_LIST_UNLOCK(&server->srv_sessions);
}

static void *session_run(void *data)
{
	struct server_session *srv_session = data;

	sccp_session_run(srv_session->session);

	/* don't check the result; not being able to queue the message is normal,
	 * and it will happen on server destroy
	 */
	server_queue_msg_session_end(&global_server, srv_session);

	return NULL;
}

static int server_start_session(struct server *server, struct server_session *srv_session)
{
	int ret;

	ret = ast_pthread_create(&srv_session->thread, NULL, session_run, srv_session);
	if (ret) {
		ast_log(LOG_ERROR, "server start session failed: pthread create: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static void server_join_sessions(struct server *server)
{
	struct server_session *srv_session;
	int ret;

	AST_LIST_LOCK(&server->srv_sessions);
	AST_LIST_TRAVERSE_SAFE_BEGIN(&server->srv_sessions, srv_session, list) {
		ast_debug(1, "joining session %p thread\n", srv_session->session);
		ret = pthread_join(srv_session->thread, NULL);
		if (ret) {
			ast_log(LOG_ERROR, "server join sessions failed: pthread_join: %s\n", strerror(ret));
		}

		AST_LIST_REMOVE_CURRENT(list);
		server_session_destroy(srv_session);
	}
	AST_LIST_TRAVERSE_SAFE_END;
	AST_LIST_UNLOCK(&server->srv_sessions);
}

static void server_on_session_end(struct server *server, struct server_session *srv_session)
{
	int ret;

	ast_debug(1, "joining session %p thread\n", srv_session->session);
	ret = pthread_join(srv_session->thread, NULL);
	if (ret) {
		ast_log(LOG_ERROR, "server on session end failed: pthread_join: %s\n", strerror(ret));
	}

	server_remove_srv_session(server, srv_session);
	server_session_destroy(srv_session);
}

static void server_process_queue(struct server *server)
{
	struct server_msg msg;

	for (;;) {
		ast_mutex_lock(&server->lock);
		if (sccp_queue_is_empty(server->queue)) {
			ast_mutex_unlock(&server->lock);
			return;
		}

		sccp_queue_get(server->queue, &msg);
		ast_mutex_unlock(&server->lock);

		switch (msg.id) {
		case MSG_RELOAD:
			server_reload_sessions(server, msg.data.reload.cfg);
			break;
		case MSG_SESSION_END:
			server_on_session_end(server, msg.data.session_end.srv_session);
			break;
		case MSG_STOP:
			server->quit = 1;
			break;
		default:
			ast_log(LOG_ERROR, "server process queue: got unknown msg id %d\n", msg.id);
			break;
		}

		server_msg_destroy(&msg);
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

	server->sockfd = new_server_socket(cfg);
	if (server->sockfd == -1) {
		return -1;
	}

	server->state = STATE_RUNNING;
	ret = ast_pthread_create_background(&server->thread, NULL, server_run, server);
	if (ret) {
		ast_log(LOG_ERROR, "server start failed: pthread create: %s\n", strerror(ret));
		close(server->sockfd);
		server->state = STATE_INITIALIZED;
	}

	return 0;
}

static void *server_run(void *data)
{
	struct server *server = data;
	struct sccp_session *session;
	struct server_session *srv_session;
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

			/* on success, the srv_session will own the session reference */
			srv_session = server_session_create(session);
			if (!srv_session) {
				ao2_ref(session, -1);
				goto end;
			}

			server_add_srv_session(server, srv_session);
			if (server_start_session(server, srv_session)) {
				server_remove_srv_session(server, srv_session);
				server_session_destroy(srv_session);
				goto end;
			}
		} else if (fds[0].revents) {
			/* unexpected events */
			goto end;
		}
	}

end:
	ast_debug(1, "server thread is leaving\n");

	close(server->sockfd);

	ast_mutex_lock(&server->lock);
	server->state = STATE_ENDED;
	ast_mutex_unlock(&server->lock);

	/* no need to lock the server since msg are only queued when the server state is
	 * STATE_RUNNING
	 */
	server_empty_queue(server);

	return NULL;
}

static char *cli_show_sessions(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct server_session *srv_session;
	int total = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "sccp show sessions";
		e->usage = "Usage: sccp show sessions\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	AST_LIST_LOCK(&global_server.srv_sessions);
	AST_LIST_TRAVERSE(&global_server.srv_sessions, srv_session, list) {
		/* note that, at the time of writing this, we can't access the content
		 * of srv_session since locking the list of srv_sessions does not guarantee
		 * that the srv_session won't change
		 */
		total++;
	}
	AST_LIST_UNLOCK(&global_server.srv_sessions);

	ast_cli(a->fd, "%d active sessions\n", total);

	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_entries[] = {
	AST_CLI_DEFINE(cli_show_sessions, "Show the active sessions"),
};

int sccp_server_init(void)
{
	if (global_server.state != STATE_UNINITIALIZED) {
		ast_log(LOG_ERROR, "sccp server init failed: server already initialized\n");
		return -1;
	}

	if (server_init(&global_server)) {
		return -1;
	}

	ast_cli_register_multiple(cli_entries, ARRAY_LEN(cli_entries));

	return 0;
}

void sccp_server_destroy(void)
{
	if (global_server.state == STATE_UNINITIALIZED) {
		ast_log(LOG_ERROR, "sccp server destroy failed: server not initialzed\n");
		return;
	}

	ast_cli_unregister_multiple(cli_entries, ARRAY_LEN(cli_entries));

	if (global_server.state != STATE_INITIALIZED) {
		if (server_queue_msg_stop(&global_server)) {
			ast_log(LOG_WARNING, "sccp server destroy error: could not ask server to stop\n");
		}

		server_join(&global_server);
		server_stop_sessions(&global_server);
		server_join_sessions(&global_server);
	}

	server_destroy(&global_server);
}

int sccp_server_start(struct sccp_cfg *cfg)
{
	if (!cfg) {
		ast_log(LOG_ERROR, "sccp server start failed: cfg is null\n");
		return -1;
	}

	if (global_server.state != STATE_INITIALIZED) {
		ast_log(LOG_ERROR, "sccp server start failed: not initialized or already started\n");
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

	if (server_queue_msg_reload(&global_server, cfg)) {
		ast_log(LOG_WARNING, "sccp server reload config failed: could not ask server to reload config\n");
		return -1;
	}

	return 0;
}

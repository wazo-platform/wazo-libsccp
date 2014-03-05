#include <errno.h>

#include <asterisk.h>
#include <asterisk/astobj2.h>
#include <asterisk/linkedlists.h>
#include <asterisk/network.h>

#include "sccp_config.h"
#include "sccp_queue.h"
#include "sccp_server.h"
#include "sccp_session.h"

#define SERVER_PORT 2000
#define SERVER_BACKLOG 50

static void *server_run(void *data);

enum server_state {
	STATE_CREATED,
	STATE_STARTED,
};

struct sccp_server {
	enum server_state state;
	int sockfd;
	int stop;

	pthread_t thread;

	struct sccp_cfg *cfg;
	struct sccp_device_registry *registry;
	struct sccp_sync_queue *sync_q;
	AST_LIST_HEAD_NOLOCK(, server_session) srv_sessions;
};

struct server_session {
	AST_LIST_ENTRY(server_session) list;
	struct sccp_server *server;
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

/*
 * On success, the function take ownership of the session (i.e. steal the reference)
 */
static struct server_session *server_session_create(struct sccp_session *session, struct sccp_server *server)
{
	struct server_session *srv_session;

	srv_session = ast_calloc(1, sizeof(*srv_session));
	if (!srv_session) {
		return NULL;
	}

	srv_session->session = session;
	srv_session->server = server;

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
	}
}

static void server_close_queue(struct sccp_server *server)
{
	sccp_sync_queue_close(server->sync_q);
}

static void server_empty_queue(struct sccp_server *server)
{
	struct sccp_queue q;
	struct server_msg msg;

	sccp_sync_queue_get_all(server->sync_q, &q);
	while (!sccp_queue_get(&q, &msg)) {
		server_msg_destroy(&msg);
	}

	sccp_queue_destroy(&q);
}

static int server_queue_msg(struct sccp_server *server, struct server_msg *msg)
{
	int ret;

	ret = sccp_sync_queue_put(server->sync_q, msg);
	if (ret) {
		server_msg_destroy(msg);
	}

	return ret;
}

static int server_queue_msg_reload(struct sccp_server *server, struct sccp_cfg *cfg)
{
	struct server_msg msg;

	server_msg_init_reload(&msg, cfg);

	return server_queue_msg(server, &msg);
}

static int server_queue_msg_session_end(struct sccp_server *server, struct server_session *srv_session)
{
	struct server_msg msg;

	server_msg_init_session_end(&msg, srv_session);

	return server_queue_msg(server, &msg);
}

static int server_queue_msg_stop(struct sccp_server *server)
{
	struct server_msg msg;

	server_msg_init_stop(&msg);

	return server_queue_msg(server, &msg);
}

static int server_join(struct sccp_server *server)
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

static void server_stop_sessions(struct sccp_server *server)
{
	struct server_session *srv_session;

	AST_LIST_TRAVERSE(&server->srv_sessions, srv_session, list) {
		/* if sccp_session_stop returns a failure, then right now, we'll
		 * have some kind of partial deadlock since there is no hard guarantee that
		 * the session will eventually stop
		 */
		sccp_session_stop(srv_session->session);
	}
}

static void server_reload_sessions(struct sccp_server *server, struct sccp_cfg *cfg)
{
	struct server_session *srv_session;

	AST_LIST_TRAVERSE(&server->srv_sessions, srv_session, list) {
		sccp_session_reload_config(srv_session->session, cfg);
	}
}

static void server_add_srv_session(struct sccp_server *server, struct server_session *srv_session)
{
	AST_LIST_INSERT_TAIL(&server->srv_sessions, srv_session, list);
}

static void server_remove_srv_session(struct sccp_server *server, struct server_session *srv_session)
{
	AST_LIST_REMOVE(&server->srv_sessions, srv_session, list);
}

static void *session_run(void *data)
{
	struct server_session *srv_session = data;

	sccp_session_run(srv_session->session);

	/* don't check the result; not being able to queue the message is normal,
	 * and it will happen on server destroy
	 */
	server_queue_msg_session_end(srv_session->server, srv_session);

	return NULL;
}

static int server_start_session(struct sccp_server *server, struct server_session *srv_session)
{
	int ret;

	ret = ast_pthread_create(&srv_session->thread, NULL, session_run, srv_session);
	if (ret) {
		ast_log(LOG_ERROR, "server start session failed: pthread create: %s\n", strerror(errno));
		return -1;
	}

	return 0;
}

static void server_join_sessions(struct sccp_server *server)
{
	struct server_session *srv_session;
	int ret;

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

static int server_start(struct sccp_server *server)
{
	int ret;

	server->sockfd = new_server_socket(server->cfg);
	if (server->sockfd == -1) {
		return -1;
	}

	server->state = STATE_STARTED;
	ret = ast_pthread_create_background(&server->thread, NULL, server_run, server);
	if (ret) {
		ast_log(LOG_ERROR, "server start failed: pthread create: %s\n", strerror(ret));
		close(server->sockfd);
		server->state = STATE_CREATED;
	}

	return 0;
}

static void server_on_session_end(struct sccp_server *server, struct server_session *srv_session)
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

static void server_process_msg(struct sccp_server *server, struct server_msg *msg)
{
	switch (msg->id) {
	case MSG_RELOAD:
		ao2_ref(server->cfg, -1);
		server->cfg = msg->data.reload.cfg;
		ao2_ref(server->cfg, +1);

		server_reload_sessions(server, msg->data.reload.cfg);
		break;
	case MSG_SESSION_END:
		server_on_session_end(server, msg->data.session_end.srv_session);
		break;
	case MSG_STOP:
		server->stop = 1;
		break;
	}

	server_msg_destroy(msg);
}

static void server_on_queue_events(struct sccp_server *server, int events)
{
	struct sccp_queue q;
	struct server_msg msg;

	if (events & POLLIN) {
		sccp_sync_queue_get_all(server->sync_q, &q);
		while (!sccp_queue_get(&q, &msg)) {
			server_process_msg(server, &msg);
		}

		sccp_queue_destroy(&q);
	}

	if (events & ~POLLIN) {
		ast_log(LOG_WARNING, "server on queue events failed: unexpected event 0x%X\n", events);
		server->stop = 1;
	}
}

static void server_on_sock_events(struct sccp_server *server, int events)
{
	struct sockaddr_in addr;
	struct sccp_session *session;
	struct server_session *srv_session;
	socklen_t addrlen;
	int sockfd;

	if (events & POLLIN) {
		addrlen = sizeof(addr);
		sockfd = accept(server->sockfd, (struct sockaddr *) &addr, &addrlen);
		if (sockfd == -1) {
			ast_log(LOG_ERROR, "server on sock events failed: accept: %s\n", strerror(errno));
			server->stop = 1;
			return;
		}

		ast_verb(4, "New SCCP connection from %s:%d accepted\n", ast_inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

		session = sccp_session_create(server->cfg, server->registry, &addr, sockfd);
		if (!session) {
			close(sockfd);
			return;
		}

		/* on success, the srv_session will own the session reference */
		srv_session = server_session_create(session, server);
		if (!srv_session) {
			ao2_ref(session, -1);
			return;
		}

		server_add_srv_session(server, srv_session);
		if (server_start_session(server, srv_session)) {
			server_remove_srv_session(server, srv_session);
			server_session_destroy(srv_session);
			return;
		}
	}

	if (events & ~POLLIN) {
		ast_log(LOG_WARNING, "server on sock events failed: unexpected event 0x%X\n", events);
		server->stop = 1;
	}
}

static void *server_run(void *data)
{
	struct sccp_server *server = data;
	struct pollfd fds[2];
	int nfds;

	if (listen(server->sockfd, SERVER_BACKLOG) == -1) {
		ast_log(LOG_ERROR, "server run failed: listen: %s\n", strerror(errno));
		goto end;
	}

	fds[0].fd = server->sockfd;
	fds[0].events = POLLIN;
	fds[1].fd = sccp_sync_queue_fd(server->sync_q);
	fds[1].events = POLLIN;

	server->stop = 0;
	for (;;) {
		nfds = poll(fds, ARRAY_LEN(fds), -1);
		if (nfds == -1) {
			ast_log(LOG_ERROR, "server run failed: poll: %s\n", strerror(errno));
			goto end;
		}

		if (fds[1].revents) {
			server_on_queue_events(server, fds[1].revents);
			if (server->stop) {
				goto end;
			}
		}

		if (fds[0].revents) {
			server_on_sock_events(server, fds[0].revents);
			if (server->stop) {
				goto end;
			}
		}
	}

end:
	ast_debug(1, "server thread is leaving\n");

	close(server->sockfd);
	server_close_queue(server);
	server_empty_queue(server);

	return NULL;
}

struct sccp_server *sccp_server_create(struct sccp_cfg *cfg, struct sccp_device_registry *registry)
{
	struct sccp_server *server;

	if (!cfg) {
		ast_log(LOG_ERROR, "sccp server create failed: cfg is null\n");
		return NULL;
	}

	if (!registry) {
		ast_log(LOG_ERROR, "sccp server create failed: registry is null\n");
		return NULL;
	}

	server = ast_calloc(1, sizeof(*server));
	if (!server) {
		return NULL;
	}

	server->sync_q = sccp_sync_queue_create(sizeof(struct server_msg));
	if (!server->sync_q) {
		ast_free(server);
		return NULL;
	}

	server->state = STATE_CREATED;
	server->cfg = cfg;
	ao2_ref(cfg, +1);
	server->registry = registry;
	AST_LIST_HEAD_INIT_NOLOCK(&server->srv_sessions);

	return server;
}

void sccp_server_destroy(struct sccp_server *server)
{
	if (server->state == STATE_STARTED) {
		if (server_queue_msg_stop(server)) {
			ast_log(LOG_WARNING, "sccp server destroy error: could not ask server to stop\n");
		}

		server_join(server);
		server_stop_sessions(server);
		server_join_sessions(server);
	}

	sccp_sync_queue_destroy(server->sync_q);
	ao2_ref(server->cfg, -1);
	ast_free(server);
}

int sccp_server_start(struct sccp_server *server)
{
	if (server->state != STATE_CREATED) {
		ast_log(LOG_ERROR, "sccp server start failed: server not in initialized state\n");
		return -1;
	}

	return server_start(server);
}

int sccp_server_reload_config(struct sccp_server *server, struct sccp_cfg *cfg)
{
	if (!cfg) {
		ast_log(LOG_ERROR, "sccp server reload config failed: cfg is null\n");
		return -1;
	}

	if (server->state != STATE_STARTED) {
		ast_log(LOG_ERROR, "sccp server reload config failed: server not in started state\n");
		return -1;
	}

	if (server_queue_msg_reload(server, cfg)) {
		ast_log(LOG_WARNING, "sccp server reload config failed: could not ask server to reload config\n");
		return -1;
	}

	return 0;
}

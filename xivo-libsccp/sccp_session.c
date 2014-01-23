#include <errno.h>

#include <asterisk.h>
#include <asterisk/astobj2.h>
#include <asterisk/network.h>
#include <asterisk/utils.h>

#include "sccp_config.h"
#include "sccp_device.h"
#include "sccp_device_registry.h"
#include "sccp_msg.h"
#include "sccp_queue.h"
#include "sccp_serializer.h"
#include "sccp_session.h"
#include "sccp_utils.h"

#define SCCP_MAX_PACKET_SZ 2000

static void sccp_session_empty_queue(struct sccp_session *session);

struct sccp_session {
	struct sccp_deserializer deserializer;
	struct sccp_serializer serializer;
	int sockfd;
	int stop;

	struct sccp_cfg *cfg;
	struct sccp_device_registry *registry;
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
	ao2_ref(session->cfg, -1);
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

struct sccp_session *sccp_session_create(struct sccp_cfg *cfg, struct sccp_device_registry *registry, int sockfd)
{
	struct sccp_queue *queue;
	struct sccp_session *session;

	if (!cfg) {
		ast_log(LOG_ERROR, "sccp session create failed: cfg is null\n");
		return NULL;
	}

	if (!registry) {
		ast_log(LOG_ERROR, "sccp session create failed: registry is null\n");
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

	sccp_deserializer_init(&session->deserializer, sockfd);
	sccp_serializer_init(&session->serializer, sockfd);
	session->sockfd = sockfd;
	session->queue = queue;
	session->stop = 0;
	session->device = NULL;
	session->cfg = cfg;
	ao2_ref(cfg, +1);
	session->registry = registry;

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

/*
 * XXX move into sccp_config ?
 *
 * \note reference count IS incremented
 */
static struct sccp_device_cfg *find_device_cfg(struct sccp_cfg *cfg, const char *name)
{
	struct sccp_device_cfg *device_cfg;

	device_cfg = sccp_cfg_find_device(cfg, name);
	if (!device_cfg) {
		device_cfg = sccp_cfg_guest_device(cfg);
	}

	return device_cfg;
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

	device_cfg = find_device_cfg(cfg, sccp_device_name(session->device));
	if (!device_cfg) {
		session->stop = 1;
	} else {
		if (sccp_device_reload_config(session->device, device_cfg)) {
			session->stop = 1;
		}

		ao2_ref(device_cfg, -1);
	}
}

static void process_queue_cb(void *msg_data, void *arg)
{
	struct session_msg *msg = msg_data;
	struct sccp_session *session = arg;

	switch (msg->id) {
	case MSG_RELOAD:
		process_reload(session, msg->data.reload.cfg);
		break;
	case MSG_STOP:
		session->stop = 1;
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
		ast_log(LOG_WARNING, "sccp session on queue events failed: unexpected event 0x%X\n", events);
		session->stop = 1;
	}
}

static int sccp_session_read_sock(struct sccp_session *session)
{
	int ret;

	ret = sccp_deserializer_read(&session->deserializer);
	if (!ret) {
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

	device_cfg = find_device_cfg(session->cfg, name);
	if (!device_cfg) {
		ast_log(LOG_WARNING, "Device is not configured [%s]\n", name);
		sccp_serializer_push_register_rej(&session->serializer);
		return;
	}

	device_info.name = name;
	device_info.type = letohl(msg->data.reg.type);
	device_info.proto_version = letohl(msg->data.reg.protoVersion);
	device = sccp_device_create(device_cfg, session, &device_info);
	ao2_ref(device_cfg, -1);
	if (!device) {
		sccp_serializer_push_register_rej(&session->serializer);
		return;
	}

	ret = sccp_device_registry_add(session->registry, device);
	if (ret) {
		if (ret == SCCP_DEVICE_REGISTRY_ALREADY) {
			ast_log(LOG_WARNING, "Device already registered [%s]\n", name);
		}

		sccp_serializer_push_register_rej(&session->serializer);
		sccp_device_destroy(device);
		ao2_ref(device, -1);
		return;
	}

	/* XXX missing session ip address and port */
	ast_verb(3, "Registered SCCP(%d) '%s' at %s:%d\n", device_info.proto_version, name, "unknown", -1);

	sccp_device_on_registration_success(device);

	/* steal the reference ownership */
	session->device = device;
}

static void sccp_session_handle_msg(struct sccp_session *session, struct sccp_msg *msg)
{
	uint32_t msg_id = letohl(msg->id);

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

		if (session->stop || session->serializer.error) {
			goto end;
		}

		if (fds[1].revents) {
			sccp_session_on_queue_events(session, fds[1].revents);
			if (session->stop || session->serializer.error) {
				goto end;
			}
		}

		if (fds[0].revents) {
			sccp_session_on_sock_events(session, fds[0].revents);
			if (session->stop || session->serializer.error) {
				goto end;
			}
		}
	}

end:
	ast_log(LOG_DEBUG, "leaving session %d thread\n", session->sockfd);

	sccp_session_close_queue(session);
	sccp_session_empty_queue(session);

	if (session->device) {
		sccp_device_registry_remove(session->registry, session->device);
		sccp_device_destroy(session->device);

		ao2_ref(session->device, -1);
		session->device = NULL;
	}
}

int sccp_session_stop(struct sccp_session *session)
{
	/* set session->stop to 1 here so that if called from the session thread,
	 * the flag will be set when going back in the session_run
	 */
	session->stop = 1;

	/* XXX we could queue a generic "progress" msg now that session->stop is set here*/
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

struct sccp_serializer *sccp_session_serializer(struct sccp_session *session)
{
	return &session->serializer;
}

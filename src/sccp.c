
#include <asterisk.h>
#include <asterisk/linkedlists.h>
#include <asterisk/poll-compat.h>
#include <asterisk/utils.h>

#include <netinet/tcp.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "device.h"
#include "message.h"
#include "sccp.h"
#include "utils.h"

#define SCCP_PORT "2000"
#define SCCP_BACKLOG 50

#define SCCP_MAX_PACKET_SZ 1000

static struct sccp_server {

	int sockfd;
	struct addrinfo *res;
	pthread_t thread_accept;
	pthread_t thread_session;

} sccp_srv = {0};

struct sccp_session {

	ast_mutex_t lock;
	pthread_t tid;
	time_t start_time;
	int sockfd;

	char *ipaddr;
	struct sccp_device *device;

	char inbuf[SCCP_MAX_PACKET_SZ];
	char outbuf[SCCP_MAX_PACKET_SZ];

	AST_LIST_ENTRY(sccp_session) list;
};

extern struct sccp_configs sccp_cfg; /* global */
static AST_LIST_HEAD_STATIC(list_session, sccp_session);

int transmit_message(struct sccp_msg *msg, struct sccp_session *session)
{
	ssize_t nbyte;

	memcpy(session->outbuf, msg, 12);
	memcpy(session->outbuf+12, &msg->data, letohl(msg->length));

	nbyte = write(session->sockfd, session->outbuf, letohl(msg->length)+8);	

	ast_log(LOG_NOTICE, "write %d bytes\n", nbyte);

	ast_free(msg);

	return 0;
}

static int handle_softkey_template_req_message(struct sccp_msg *msg, struct sccp_session *session)
{
	msg = msg_alloc(sizeof(struct softkey_template_res_message), SOFTKEY_TEMPLATE_RES_MESSAGE);

	msg->data.softkeytemplate.softKeyOffset = htolel(0);
	msg->data.softkeytemplate.softKeyCount = htolel(sizeof(softkey_template_default) / sizeof(struct softkey_template_definition));
	msg->data.softkeytemplate.totalSoftKeyCount = htolel(sizeof(softkey_template_default) / sizeof(struct softkey_template_definition));
	memcpy(msg->data.softkeytemplate.softKeyTemplateDefinition, softkey_template_default, sizeof(softkey_template_default));

	transmit_message(msg, session);

	return 0;
}

static int handle_button_template_req_message(struct sccp_msg *msg, struct sccp_session *session)
{
	struct button_definition_template btl[42];
	int buttonCount = 0;
	int i;

	msg = msg_alloc(sizeof(struct button_template_res_message), BUTTON_TEMPLATE_RES_MESSAGE);
	memset(&btl, 0, sizeof(btl));

	device_get_button_template(session->device, btl);

	for (i=0; i<42; i++) {
		switch (btl[i].buttonDefinition) {
			case BT_CUST_LINESPEEDDIAL:
				msg->data.buttontemplate.definition[i].buttonDefinition = BT_NONE;
				msg->data.buttontemplate.definition[i].instanceNumber = htolel(0);
				buttonCount++;
				break;

			case BT_NONE:
				break;

			default:
				msg->data.buttontemplate.definition[i].buttonDefinition = htolel(btl[i].buttonDefinition);
				msg->data.buttontemplate.definition[i].instanceNumber = htolel(0);
				break;
		}
	}

	msg->data.buttontemplate.buttonOffset = htolel(0);
	msg->data.buttontemplate.buttonCount = htolel(buttonCount);
	msg->data.buttontemplate.totalButtonCount = htolel(buttonCount);

	transmit_message(msg, session);
	
	return 0;
}

static int handle_keep_alive_message(struct sccp_msg *msg, struct sccp_session *session)
{
	msg = msg_alloc(0, KEEP_ALIVE_ACK_MESSAGE);
	transmit_message(msg, session);

	return 0;
}

static int register_device(struct sccp_msg *msg, struct sccp_session *session)
{
	struct sccp_device *device_itr;
	int device_found = 0;

	AST_LIST_TRAVERSE(&list_device, device_itr, list) {

		if (!strcasecmp(device_itr->name, msg->data.reg.name)) {
			ast_log(LOG_NOTICE, "Found device [%s]\n", device_itr->name);

			device_itr->registered = 1;
			device_itr->type = letohl(msg->data.reg.type);
			session->device = device_itr;

			device_found = 1;
			break;
		}	
	}

	return device_found;
}

static int handle_softkey_set_req_message(struct sccp_msg *msg, struct sccp_session *session)
{
	const struct softkey_definitions *softkeymode = softkey_default_definitions;
	int keyset_count;
	int i, j, k;

	msg = msg_alloc(sizeof(struct softkey_set_res_message), SOFTKEY_SET_RES_MESSAGE);
	keyset_count = sizeof(softkey_default_definitions) / sizeof(struct softkey_definitions);

        msg->data.softkeysets.softKeySetOffset = htolel(0);
        msg->data.softkeysets.softKeySetCount = htolel(keyset_count);
        msg->data.softkeysets.totalSoftKeySetCount = htolel(keyset_count);

	for (i = 0; i < keyset_count; i++) {

		for (j = 0; j < softkeymode->count; j++) {
			msg->data.softkeysets.softKeySetDefinition[softkeymode->mode].softKeyTemplateIndex[j] = htolel(softkeymode->defaults[j]);
			msg->data.softkeysets.softKeySetDefinition[softkeymode->mode].softKeyInfoIndex[j] = htolel(softkeymode->defaults[j]);
		}
		softkeymode++;
	}

	transmit_message(msg, session);

	msg = msg_alloc(sizeof(struct select_soft_keys_message), SELECT_SOFT_KEYS_MESSAGE);
        msg->data.selectsoftkey.instance = htolel(0);
        msg->data.selectsoftkey.reference = htolel(0);
        msg->data.selectsoftkey.softKeySetIndex = htolel(KEYDEF_ONHOOK);
        msg->data.selectsoftkey.validKeyMask = htolel(0xFFFFFFFF);

	transmit_message(msg, session);

	return 0;
}

static int handle_register_message(struct sccp_msg *msg, struct sccp_session *session)
{
	int ret;


	ast_log(LOG_NOTICE, "name %s\n", msg->data.reg.name);
	ast_log(LOG_NOTICE, "userId %d\n", msg->data.reg.userId);
	ast_log(LOG_NOTICE, "instance %d\n", msg->data.reg.instance);
	ast_log(LOG_NOTICE, "ip %d\n", msg->data.reg.ip);
	ast_log(LOG_NOTICE, "type %d\n", msg->data.reg.type);
	ast_log(LOG_NOTICE, "maxStreams %d\n", msg->data.reg.maxStreams);


	ret = register_device(msg, session);
	if (ret == 0) {
		ast_log(LOG_ERROR, "Rejecting [%s], device not found\n", msg->data.reg.name);
		msg = msg_alloc(sizeof(struct register_rej_message), REGISTER_REJ_MESSAGE);

		if (msg == NULL) {
			return -1;
		}

		snprintf(msg->data.regrej.errMsg, sizeof(msg->data.regrej.errMsg), "Access denied: %s\n", msg->data.reg.name);
		transmit_message(msg, session);

		return 0;
	}

	msg = msg_alloc(sizeof(struct register_ack_message), REGISTER_ACK_MESSAGE);
	if (msg == NULL) {
		return -1;
	}

        msg->data.regack.res[0] = '0';
        msg->data.regack.res[1] = '\0';
        msg->data.regack.keepAlive = htolel(sccp_cfg.keepalive);
        memcpy(msg->data.regack.dateTemplate, sccp_cfg.dateformat, sizeof(msg->data.regack.dateTemplate));
        msg->data.regack.res2[0] = '0';
        msg->data.regack.res2[1] = '\0';
        msg->data.regack.secondaryKeepAlive = htolel(sccp_cfg.keepalive);

        transmit_message(msg, session);

	return 0;
}


static void destroy_session(struct sccp_session **session)
{
	ast_mutex_destroy(&(*session)->lock);
	ast_free((*session)->ipaddr);
	close((*session)->sockfd);
	ast_free(*session);
}

static int handle_message(struct sccp_msg *msg, struct sccp_session *session)
{
	switch (msg->id) {

		case KEEP_ALIVE_MESSAGE:
			ast_log(LOG_NOTICE, "Keep alive message\n");
			handle_keep_alive_message(msg, session);
			break;

		case REGISTER_MESSAGE:
			ast_log(LOG_NOTICE, "Register message\n");
			handle_register_message(msg, session);
			break;

		case BUTTON_TEMPLATE_REQ_MESSAGE:
			ast_log(LOG_NOTICE, "Button template request message\n");
			handle_button_template_req_message(msg, session);
			break;

		case SOFTKEY_TEMPLATE_REQ_MESSAGE:
			ast_log(LOG_NOTICE, "Softkey template request message\n");
			handle_softkey_template_req_message(msg, session);
			break;

		case ALARM_MESSAGE:
			ast_log(LOG_NOTICE, "Alarm Message: %s\n", msg->data.alarm.displayMessage);
			break;

		case SOFTKEY_SET_REQ_MESSAGE:
			ast_log(LOG_NOTICE, "Softkey set request message\n");
			handle_softkey_set_req_message(msg, session);
			break;

		default:
			ast_log(LOG_NOTICE, "Unknown message %d\n", msg->id);
			break;
	}
	return 0;
}

static int fetch_data(struct sccp_session *session)
{
	struct pollfd fds[1];
	int nfds = 0;
	time_t now = 0;
	ssize_t nbyte = 0;
	int msg_len = 0;
	
	time(&now);
	/* if no device or device is not registered and time has elapsed */
	if ((session->device == NULL || (session->device != NULL && session->device->registered == 0))
		&& now > session->start_time + sccp_cfg.authtimeout) {
		return -1;
	}

	fds[0].fd = session->sockfd;
	fds[0].events = POLLIN;
	fds[0].revents = 0;

	nfds = ast_poll(fds, 1, sccp_cfg.keepalive * 1000); /* millisecond */
	if (nfds == -1) { /* something wrong happend */
		ast_log(LOG_WARNING, "Failed to poll socket: %s\n", strerror(errno));
		return -1;

	} else if (nfds == 0) { /* the file descriptor is not ready */
		ast_log(LOG_WARNING, "Device has timed out\n");
		return -1;

	} else if (fds[0].revents & POLLERR || fds[0].revents & POLLHUP) {
		ast_log(LOG_WARNING, "Device has closed the connection\n");
		return -1;

	} else if (fds[0].revents & POLLIN || fds[0].revents & POLLPRI) {

		/* fetch the field that contain the packet length */
		nbyte = read(session->sockfd, session->inbuf, 4);
		if (nbyte < 0) { /* something wrong happend */
			ast_log(LOG_WARNING, "Failed to read socket: %s\n", strerror(errno));
			return -1;

		} else if (nbyte == 0) { /* EOF */
			ast_log(LOG_NOTICE, "Device has closed the connection\n");
			return -1;

		} else if (nbyte < 4) {
			ast_log(LOG_WARNING, "Client sent less data than expected. Expected 4 but got %d\n", nbyte);
			return -1;
		}

		msg_len = letohl(*((int *)session->inbuf));
		if (msg_len > SCCP_MAX_PACKET_SZ || msg_len < 0) {
			ast_log(LOG_WARNING, "Packet length is out of bounds: 0 > %d > %d\n", msg_len, SCCP_MAX_PACKET_SZ);
			return -1;
		}

		/* bypass the length field and fetch the payload */
		nbyte = read(session->sockfd, session->inbuf+4, msg_len+4);
		if (nbyte < 0) {
			ast_log(LOG_WARNING, "Failed to read socket: %s\n", strerror(errno));
			return -1;

		} else if (nbyte == 0) { /* EOF */
			ast_log(LOG_NOTICE, "Device has closed the connection\n");
			return -1;
		}

		return nbyte;
	}

	return -1;	
}

static void *thread_session(void *data)
{
	int ret = 0;
	struct sccp_session *session = data;
	struct sccp_msg *msg;
	int connected = 1;

	while (connected) {

		ret = fetch_data(session);

		if (ret == -1) {
			/* something wrong happend */
			connected = 0;
			AST_LIST_LOCK(&list_session);
			session = AST_LIST_REMOVE(&list_session, session, list);
			AST_LIST_UNLOCK(&list_session);	
		
		} else if (ret > 0) {
			/* we've read some data */			
			msg = (struct sccp_msg *)session->inbuf;
			ret = handle_message(msg, session);
		}
	}

	if (session)
		destroy_session(&session);

	return 0;
}

static void *thread_accept(void *data)
{
	int new_sockfd;
	struct sockaddr_in addr;
	struct sccp_session *session = NULL;
	socklen_t addrlen;
	int flag_nodelay = 1;
	data = NULL;

	while (1) {

		addrlen = sizeof(addr);
		new_sockfd = accept(sccp_srv.sockfd, (struct sockaddr *)&addr, &addrlen);
		if (new_sockfd == -1) {
			ast_log(LOG_ERROR, "Failed to accept new connection: %s... "
						"the main thread is going down now\n", strerror(errno));
			return;
		}

		/* send multiple buffers as individual packets */
		setsockopt(new_sockfd, IPPROTO_TCP, TCP_NODELAY, &flag_nodelay, sizeof(flag_nodelay));

		/* session constructor */	
		session = ast_calloc(1, sizeof(struct sccp_session));
		if (session == NULL) {
			close(new_sockfd);
			continue;
		}

		session->tid = AST_PTHREADT_NULL; 
		session->sockfd = new_sockfd;
		session->ipaddr = ast_strdup(ast_inet_ntoa(addr.sin_addr));
		ast_mutex_init(&session->lock);
		time(&session->start_time);
	
		AST_LIST_LOCK(&list_session);
		AST_LIST_INSERT_HEAD(&list_session, session, list);
		AST_LIST_UNLOCK(&list_session);

		ast_log(LOG_NOTICE, "A new device has connected from: %s\n", session->ipaddr);
		ast_pthread_create_background(&session->tid, NULL, thread_session, session);
	}
}

void sccp_server_fini()
{
	struct sccp_session *session_itr = NULL;

	int ret;

	ret = pthread_cancel(sccp_srv.thread_accept);
	ret = pthread_kill(sccp_srv.thread_accept, SIGURG);
	ret = pthread_join(sccp_srv.thread_accept, NULL);

	AST_LIST_TRAVERSE_SAFE_BEGIN(&list_session, session_itr, list) {
		if (session_itr != NULL) {

			ast_log(LOG_NOTICE, "Session del %s\n", session_itr->ipaddr);
			AST_LIST_REMOVE_CURRENT(&list_session, list);

			pthread_cancel(session_itr->tid);
			pthread_kill(session_itr->tid, SIGURG);
			pthread_join(session_itr->tid, NULL);

			destroy_session(&session_itr);
		}
	}
	AST_LIST_TRAVERSE_SAFE_END;

	freeaddrinfo(sccp_srv.res);
	shutdown(sccp_srv.sockfd, SHUT_RDWR);
}

int sccp_server_init(void)
{
	int ret = 0;
	const int flag_reuse = 1;
	struct addrinfo hints;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICHOST;

	getaddrinfo(sccp_cfg.bindaddr, SCCP_PORT, &hints, &sccp_srv.res);

	sccp_srv.sockfd = socket(sccp_srv.res->ai_family, sccp_srv.res->ai_socktype, sccp_srv.res->ai_protocol);
	setsockopt(sccp_srv.sockfd, SOL_SOCKET, SO_REUSEADDR, &flag_reuse, sizeof(flag_reuse));

	ret = bind(sccp_srv.sockfd, sccp_srv.res->ai_addr, sccp_srv.res->ai_addrlen);
	if (ret == -1) {
		ast_log(LOG_ERROR, "Failed to bind socket: %s\n", strerror(errno));
		return -1;
	}

	ret = listen(sccp_srv.sockfd, SCCP_BACKLOG);
	if (ret == -1) {
		ast_log(LOG_ERROR, "Failed to listen socket: %s\n", strerror(errno));
		return -1;
	}

	ast_pthread_create_background(&sccp_srv.thread_accept, NULL, thread_accept, NULL);

	return 0;
}

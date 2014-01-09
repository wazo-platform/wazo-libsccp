#include <asterisk.h>
#include <asterisk/astobj2.h>

#include "sccp_session.h"

struct sccp_session {
	int sockfd;
};

static void sccp_session_destructor(void *data)
{
	struct sccp_session *session = data;

	ast_log(LOG_DEBUG, "in destructor for session %p\n", session);

	close(session->sockfd);
}

struct sccp_session *sccp_session_create(int sockfd)
{
	struct sccp_session *session;

	session = ao2_alloc(sizeof(*session), sccp_session_destructor);
	if (!session) {
		return NULL;
	}

	ast_log(LOG_DEBUG, "session %p created\n", session);

	session->sockfd = sockfd;

	return session;
}

void sccp_session_run(struct sccp_session *session)
{
	sleep(4);
}

int sccp_session_stop(struct sccp_session *session)
{
	return 0;
}

int sccp_session_reload_config(struct sccp_session *session)
{
	return 0;
}
